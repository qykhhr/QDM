"""本机 HTTP/TLS 吞吐对照；固定数据源、落盘和检查点，保留原始事件。

TLS 使用临时自签名证书，通过 --ca-file 显式信任；不关闭验证、不修改系统 CA。
对照包含真实 QFile 写入、SHA-256 和可靠检查点，不是只接收到内存的测速。
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import shutil
import ssl
import subprocess
import threading
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
BLOCK = bytes(range(256))*256
SIZE = 1024*1024*1024


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        r = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
        start, end = (int(r[1]), min(int(r[2]), SIZE-1)) if r else (0, SIZE-1)
        self.send_response(206 if r else 200)
        self.send_header('ETag', '"benchmark-v1"')
        self.send_header('Content-Length', str(end-start+1))
        if r:
            self.send_header('Content-Range', f'bytes {start}-{end}/{SIZE}')
        self.end_headers()
        try:
            while start <= end:
                n = min(len(BLOCK)-start%len(BLOCK), end-start+1)
                self.wfile.write(BLOCK[start%len(BLOCK):start%len(BLOCK)+n])
                start += n
        except (OSError, ConnectionError):
            pass


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True

    def handle_error(self, request, client_address):
        import sys
        if not isinstance(sys.exception(), OSError):
            super().handle_error(request, client_address)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--exe', type=pathlib.Path, required=True)
    p.add_argument('--tls', action='store_true')
    p.add_argument('--seconds', type=int, default=5)
    p.add_argument('--connections', default='4,8,16,64')
    p.add_argument('--complete', action='store_true', help='Download complete file and verify SHA-256')
    p.add_argument('--size-mib', type=int, default=1024)
    p.add_argument('--check-untrusted', action='store_true', help='TLS must fail without the explicit test CA')
    args = p.parse_args()
    global SIZE
    SIZE = args.size_mib*1024*1024+317
    work = ROOT/'tests/work'/('benchmark-'+uuid.uuid4().hex[:10])
    work.mkdir(parents=True)
    server = Server(('127.0.0.1', 0), Handler)
    cert = work/'cert.pem'
    if args.tls:
        openssl = shutil.which('openssl')
        if not openssl:
            raise RuntimeError('TLS benchmark requires openssl')
        config = work/'openssl.cnf'
        config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n'
                          '[dn]\nCN=QDM local benchmark\n[ext]\nsubjectAltName=IP:127.0.0.1\n'
                          'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,digitalSignature,keyEncipherment\n', encoding='ascii')
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(work/'key.pem'), '-out', str(cert), '-config', str(config)],
                       check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, work/'key.pem')
        server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = []
    try:
        if args.check_untrusted:
            assert args.tls, '--check-untrusted requires --tls'
            folder = work/'untrusted'
            folder.mkdir()
            rejected = subprocess.run([str(args.exe.resolve()), '--headless', '--url',
                f'https://127.0.0.1:{server.server_port}/file', '--output', str(folder/'output.bin'),
                '--data-dir', str(folder/'data')], capture_output=True, timeout=30)
            assert rejected.returncode == 1 and not (folder/'output.bin').exists(), rejected
            print('untrusted TLS certificate: rejected', flush=True)
        for connections in map(int, args.connections.split(',')):
            folder = work/str(connections)
            folder.mkdir()
            command = [str(args.exe.resolve()), '--headless', '--url',
                       f'{"https" if args.tls else "http"}://127.0.0.1:{server.server_port}/file',
                       '--output', str(folder/'output.bin'), '--data-dir', str(folder/'data'),
                       '--connections', str(connections)]
            if not args.complete:
                command += ['--pause-after', str(args.seconds*1000)]
            if args.tls:
                command += ['--ca-file', str(cert)]
            events = []
            started = time.monotonic()
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            # 时间戳由测试进程记录，不依赖 GUI 瞬时速度或客户端轮询频率。
            for line in process.stdout:
                event = json.loads(line)
                event['wall_seconds'] = time.monotonic()-started
                events.append(event)
            _, stderr = process.communicate(timeout=15)
            assert process.returncode == 0, (process.returncode, stderr, events[-3:])
            samples = [e for e in events if e['state']=='Downloading' and e['wall_seconds']>=1]
            if len(samples)>=2:
                first, last = samples[0], samples[-1]
            else:
                # 小文件完整性验证可能不足一个稳定采样窗口，保留全程参考值。
                first = next(e for e in events if e['state']=='Downloading')
                last = events[-1]
            seconds = last['wall_seconds']-first['wall_seconds']
            rate = (int(last['received'])-int(first['received']))/seconds/(1024*1024)
            result = dict(connections=connections, segments=last['segments'], mib_per_second=round(rate, 2),
                          sample_seconds=round(seconds, 2), events=events)
            if args.complete:
                assert events[-1]['state']=='Completed', events[-1]
                expected = hashlib.sha256()
                for offset in range(0, SIZE, len(BLOCK)):
                    expected.update(BLOCK[:min(len(BLOCK), SIZE-offset)])
                with (folder/'output.bin').open('rb') as f:
                    actual = hashlib.file_digest(f, 'sha256').hexdigest()
                assert actual == expected.hexdigest(), 'Full-file SHA-256 mismatch'
                result['sha256_verified'] = True
            results.append(result)
            print(json.dumps({k:v for k,v in result.items() if k!='events'}), flush=True)
        report = dict(exe=str(args.exe.resolve()), sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                      tls=args.tls, size=SIZE, results=results)
        (work/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('Artifacts:', work, flush=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()

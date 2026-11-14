"""真实 EXE + 可控 HTTP 服务集成测试，不用公网速度推断正确性。

每个场景独立数据目录；验证输出 SHA-256、数据库检查点和 Range 起点。
崩溃测试只终止本脚本创建的子进程，不影响桌面运行的 QDM。
"""
import hashlib
import http.server
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import threading
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = pathlib.Path(os.environ.get('QDM_EXE', str(ROOT / 'build/release/bin/QDM.exe')))
WORK = ROOT / 'tests/work' / uuid.uuid4().hex[:10]
WORK.mkdir(parents=True)
BLOCK = bytes(range(256)) * 256
SIZE = 24 * 1024 * 1024 + 317
REQUESTS = []
ETAG = '"fixture-v1"'
RETRY_COUNT = 0


def expected_hash(size):
    h = hashlib.sha256()
    for offset in range(0, size, len(BLOCK)):
        h.update(BLOCK[:min(len(BLOCK), size-offset)])
    return h.hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        global RETRY_COUNT
        path = self.path.split('?')[0]
        if path == '/retry' and RETRY_COUNT == 0:
            RETRY_COUNT += 1
            self.send_response(429)
            self.send_header('Retry-After', '1')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/range')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/error':
            self.send_error(404)
            return
        size = 0 if path in ['/empty', '/empty416'] else (512*1024*1024+317 if path == '/wide' else SIZE)
        r = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
        ranged = bool(r) and path not in ['/stream', '/unknown', '/empty']
        if path == '/fallback' and r and int(r[2]) > 0:
            ranged = False
        start, end = (int(r[1]), min(int(r[2]), size-1)) if ranged else (0, size-1)
        REQUESTS.append((path, start, end, self.headers.get('Range')))
        if ranged and (start > end or start >= size):
            self.send_response(416)
            self.send_header('Content-Range', f'bytes */{size}')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        self.send_response(206 if ranged else 200)
        self.send_header('ETag', ETAG)
        if ranged:
            # 探测正确、实际下载头错误，确认错误正文绝不写入输出。
            reported_start = start + 1 if path == '/bad-range' and end > start else start
            self.send_header('Content-Range', f'bytes {reported_start}-{end}/{size}')
        if path == '/unknown':
            self.send_header('Connection', 'close')
            self.close_connection = True
        else:
            self.send_header('Content-Length', str(end-start+1))
        self.end_headers()
        try:
            pos = start
            while pos <= end:
                count = min(len(BLOCK)-(pos % len(BLOCK)), end-pos+1)
                self.wfile.write(BLOCK[pos % len(BLOCK):pos % len(BLOCK)+count])
                self.wfile.flush()
                pos += count
                if path == '/slow':
                    time.sleep(.025)
        except (ConnectionError, OSError):
            pass


class FixtureServer(http.server.ThreadingHTTPServer):
    request_queue_size = 128  # 64 路建连不应受默认 5 个 backlog 干扰。

    def handle_error(self, request, client_address):
        # 探测主动中断正文、暂停和强制退出均可能触发预期的连接重置。
        import sys
        if not isinstance(sys.exception(), (ConnectionError, OSError)):
            super().handle_error(request, client_address)


server = FixtureServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
BASE = f'http://127.0.0.1:{server.server_port}'


def command(name, path='/range', extra=None):
    d = WORK / name
    d.mkdir(exist_ok=True)
    return [str(EXE), '--headless', '--url', BASE+path, '--output', str(d/'output.bin'),
            '--data-dir', str(d/'data'), '--connections', '4'] + (extra or [])


def run(args, success=True):
    result = subprocess.run(args, capture_output=True, timeout=45)
    events = [json.loads(line) for line in result.stdout.decode('utf-8').splitlines() if line.startswith('{')]
    assert result.returncode == (0 if success else 1), (result.returncode, result.stdout[-3000:], result.stderr)
    assert events, result.stderr
    return events


def verify(name, size=SIZE):
    path = WORK / name / 'output.bin'
    assert path.stat().st_size == size
    with path.open('rb') as f:
        actual = hashlib.file_digest(f, 'sha256').hexdigest()
    assert actual == expected_hash(size), (name, actual)


def resume(name):
    d = WORK/name
    with sqlite3.connect(d/'data/tasks.sqlite') as db:
        task_id, committed = db.execute('select id,committed from tasks').fetchone()
    events = run([str(EXE), '--headless', '--resume', task_id, '--data-dir', str(d/'data')])
    assert events[-1]['state'] == 'Completed', events[-1]
    verify(name)
    return committed


def main():
    global ETAG
    results = []
    for name, path, size in [('range', '/range', SIZE), ('stream', '/stream', SIZE),
                             ('unknown', '/unknown', SIZE), ('empty', '/empty', 0),
                             ('redirect', '/redirect', SIZE), ('empty416', '/empty416', 0),
                             ('fallback', '/fallback', SIZE), ('retry', '/retry', SIZE)]:
        events = run(command(name, path))
        assert events[-1]['state'] == 'Completed', events[-1]
        verify(name, size)
        if name == 'range':
            assert events[-1]['segments'] >= 2
        results.append(name + ': PASS (SHA-256)')
    for name, path in [('bad-range', '/bad-range'), ('error', '/error')]:
        events = run(command(name, path), success=False)
        assert events[-1]['state'] == 'Failed'
        assert not (WORK/name/'output.bin').exists()
        results.append(name + ': PASS (rejected)')

    run(command('pause', '/slow', ['--pause-after', '450']))
    before = len(REQUESTS)
    committed = resume('pause')
    assert 0 < committed < SIZE
    assert any(path == '/slow' and start > 0 for path, start, end, r in REQUESTS[before:])
    results.append('pause/resume: PASS (checkpoint + SHA-256)')

    args = command('crash', '/slow')
    with (WORK/'crash/events.log').open('wb') as log:
        process = subprocess.Popen(args, stdout=log, stderr=subprocess.PIPE)
        # 2 秒周期检查点提交后强制终止，模拟没有机会保存退出状态。
        deadline = time.monotonic()+15
        while time.monotonic() < deadline:
            time.sleep(.1)
            db_path = WORK/'crash/data/tasks.sqlite'
            if db_path.exists():
                with sqlite3.connect(db_path) as db:
                    row = db.execute('select committed from tasks').fetchone()
                if row and row[0] > 0:
                    break
        else:
            process.kill()
            raise AssertionError('No crash checkpoint was produced')
        process.kill()
        process.wait(timeout=10)
    assert resume('crash') > 0
    results.append('forced termination/resume: PASS (SHA-256)')

    run(command('corrupt', '/slow', ['--pause-after', '450']))
    part = next((WORK/'corrupt').glob('*.part'))
    with part.open('r+b') as f:
        f.write(b'corrupted checkpoint')
    resume('corrupt')
    results.append('corrupted checkpoint: PASS (redownload + SHA-256)')

    run(command('changed', '/slow', ['--pause-after', '450']))
    ETAG = '"fixture-v2"'
    before = len(REQUESTS)
    resume('changed')
    ranges = [(start, end) for path, start, end, r in REQUESTS[before:] if path == '/slow' and end > start]
    assert any(start == 0 for start, end in ranges)
    results.append('changed ETag: PASS (restart + SHA-256)')

    # 模拟“重命名完成、Completed 尚未提交”的窄崩溃窗口。
    with sqlite3.connect(WORK/'range/data/tasks.sqlite') as db:
        db.execute("update tasks set state='Finalizing'")
        encrypted_url = db.execute('select url from tasks').fetchone()[0]
        assert encrypted_url.startswith('dpapi:') and 'http' not in encrypted_url
    resume('range')
    results.append('final rename recovery + protected URL: PASS')

    # 已完成任务重复恢复应立即返回，不应进入无事件等待。
    resume('range')
    results.append('completed resume is idempotent: PASS')

    # 用户请求 64 路，但小文件仍按最小分段限制减少实际并发。
    events = run(command('max-config', '/range', ['--connections', '64']))
    verify('max-config')
    assert 1 <= events[-1]['segments'] <= 64
    results.append('64 limit + adaptive segment count: PASS')

    # 512 MiB 随机访问分段文件实际产生 64 段，验证高并发边界及输出。
    events = run(command('wide', '/wide', ['--connections', '64']))
    verify('wide', 512*1024*1024+317)
    assert events[-1]['segments'] == 64
    results.append('actual 64 segments / 512 MiB: PASS (SHA-256)')
    (WORK/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print('\n'.join(results))
    print('Artifacts:', WORK)


if __name__ == '__main__':
    try:
        main()
    finally:
        server.shutdown()

"""真实桌面/Native Host/HTTP 协议端到端验证，所有数据均在 tests/work。

覆盖 v1 数据库迁移、鉴权、来源校验、任务幂等、文件摘要及浏览器长度前缀。
不注册系统宿主，不读取真实浏览器资料，不终止用户桌面 QDM。
"""
import ctypes
import ctypes.wintypes
import hashlib
import http.client
import json
import pathlib
import shutil
import socket
import sqlite3
import struct
import subprocess
import time
import uuid
import integration as fixture

EXE = fixture.EXE
ID = 'nkblcdnjneadmggjpnjajlbebpbmhjpj'
WORK = fixture.WORK/'bridge'
DATA = WORK/'data'
OUTPUT = WORK/'downloads'
DATA.mkdir(parents=True)
OUTPUT.mkdir()


def token():
    class Blob(ctypes.Structure):
        _fields_ = [('size', ctypes.wintypes.DWORD), ('data', ctypes.POINTER(ctypes.c_ubyte))]
    raw = (DATA/'browser-secret.bin').read_bytes()
    buffer = (ctypes.c_ubyte*len(raw)).from_buffer_copy(raw)
    source = Blob(len(raw), buffer)
    dest = Blob()
    library = ctypes.WinDLL('crypt32', use_last_error=True)
    library.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.wintypes.DWORD, ctypes.POINTER(Blob)]
    assert library.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 1, ctypes.byref(dest))
    result = ctypes.string_at(dest.data, dest.size).decode('ascii')
    kernel = ctypes.WinDLL('kernel32')
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree(dest.data)
    return result


def start(milliseconds):
    log = (WORK/f'events-{milliseconds}.log').open('wb')
    p = subprocess.Popen([str(EXE), '--headless', '--serve', '--data-dir', str(DATA), '--download-dir', str(OUTPUT),
                          '--bridge-port', '0', '--pause-after', str(milliseconds)], stdout=log, stderr=subprocess.PIPE)
    deadline = time.monotonic()+10
    while time.monotonic()<deadline:
        if (DATA/'browser-endpoint.json').exists():
            info = json.loads((DATA/'browser-endpoint.json').read_text())
            if int(info['pid']) == p.pid:
                return p, info['port'], log
        if p.poll() is not None:
            raise AssertionError(p.communicate())
        time.sleep(.03)
    p.kill()
    raise AssertionError('No browser bridge endpoint')


def request(port, method='GET', path='/api/v1/health', data=None, headers=None):
    body = json.dumps(data).encode() if data is not None else None
    defaults = {'Authorization': 'Bearer '+token(), 'X-QDM-Extension-Id': ID,
                'Origin': 'chrome-extension://'+ID, 'Content-Type': 'application/json'}
    defaults.update(headers or {})
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
    c.request(method, path, body=body, headers=defaults)
    r = c.getresponse()
    status, result = r.status, json.loads(r.read())
    c.close()
    return status, result


def native(host, payload, origin='chrome-extension://'+ID+'/', raw=None):
    message = json.dumps(payload).encode()
    framed = raw if raw is not None else struct.pack('<I', len(message))+message
    p = subprocess.run([str(host), origin], input=framed, capture_output=True, timeout=15)
    assert len(p.stdout)>=4, (p.returncode, p.stderr)
    length = struct.unpack('<I', p.stdout[:4])[0]
    assert len(p.stdout) == length+4
    return json.loads(p.stdout[4:])


def wait_complete(port, task):
    deadline = time.monotonic()+6
    while time.monotonic()<deadline:
        status, state = request(port, path='/api/v1/tasks/'+task)
        assert status == 200
        if state['state']=='Completed':
            with (OUTPUT/state['name']).open('rb') as f:
                assert hashlib.file_digest(f, 'sha256').hexdigest() == fixture.expected_hash(fixture.SIZE)
            return state
        assert state['state'] != 'Failed', state
        time.sleep(.03)
    raise AssertionError(state)


def raw_status(port, wire):
    with socket.create_connection(('127.0.0.1',port), timeout=3) as s:
        s.sendall(wire)
        return int(s.recv(4096).split(b' ')[1])


def main():
    # 最小 v1 数据库，确认原有桌面任务不会在增加幂等字段时丢失。
    with sqlite3.connect(DATA/'tasks.sqlite') as db:
        db.execute('CREATE TABLE tasks(id TEXT PRIMARY KEY,url TEXT NOT NULL,path TEXT NOT NULL UNIQUE,state TEXT NOT NULL,error TEXT,total INTEGER,committed INTEGER,connections INTEGER,ranges INTEGER,etag TEXT,modified TEXT,segments TEXT NOT NULL)')
        db.execute('PRAGMA user_version=1')
        db.execute('INSERT INTO tasks VALUES(?,?,?,?,?,?,?,?,?,?,?,?)', ('legacy',fixture.BASE+'/range',str(OUTPUT/'legacy.bin'),'Paused','',-1,0,4,0,'','','[]'))
    first, port, log = start(12000)
    results=[]
    try:
        assert request(port)[0]==200
        assert request(port,headers={'Authorization':'Bearer wrong'})[0]==401
        assert request(port,headers={'Origin':'https://evil.example'})[0]==403
        assert request(port,headers={'Host':'evil.example'})[0]==403
        assert request(port,headers={'X-QDM-Extension-Id':'a'*32})[0]==401
        results.append('Token / Origin / Host / extension identity checks: PASS')
        good={'request_id':str(uuid.uuid4()),'url':fixture.BASE+'/range','suggested_name':'resource.bin','profile_id':'default'}
        status, created=request(port,'POST','/api/v1/tasks',good)
        assert status==201,created
        wait_complete(port,created['task_id'])
        status, same=request(port,'POST','/api/v1/tasks',good)
        assert status==200 and same['task_id']==created['task_id']
        assert request(port,'POST','/api/v1/tasks',{**good,'url':fixture.BASE+'/range?changed=1'})[0]==409
        assert request(port,'POST','/api/v1/tasks',{**good,'request_id':str(uuid.uuid4()),'path':str(WORK/'outside.bin')})[0]==400
        assert request(port,'POST','/api/v1/tasks',{**good,'request_id':str(uuid.uuid4()),'url':fixture.BASE+'/a.m3u8'})[0]==422
        results.append('HTTP create/status/dedup/conflict/unsupported fields and streams: PASS (SHA-256)')
        traversal={**good,'request_id':str(uuid.uuid4()),'suggested_name':'../../outside.bin'}
        status, clean=request(port,'POST','/api/v1/tasks',traversal)
        assert status==201,clean
        assert wait_complete(port,clean['task_id'])['name']=='outside.bin'
        assert not (WORK/'outside.bin').exists()
        results.append('Filename traversal contained inside desktop download profile: PASS')
        prefix=f'POST /api/v1/tasks HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n'.encode()
        assert raw_status(port,prefix+b'Transfer-Encoding: chunked\r\n\r\n')==400
        assert raw_status(port,prefix+b'Content-Length: 0\r\nContent-Length: 0\r\n\r\n')==400
        assert raw_status(port,prefix+b'Content-Length: 999999\r\n\r\n')==413
        assert raw_status(port,prefix+b'Content-Length: +5\r\n\r\nhello')==413
        results.append('Bounded HTTP parsing rejects chunked/duplicate/oversize/malformed lengths: PASS')
        hostdir=WORK/'host';hostdir.mkdir()
        for name in ['QDM.NativeHost.exe','Qt5Core.dll','Qt5Network.dll','msvcp120.dll','msvcr120.dll']:
            shutil.copy2(EXE.parent/name,hostdir/name)
        (hostdir/'native-host-settings.json').write_text(json.dumps({'data_dir':str(DATA)}))
        host=hostdir/'QDM.NativeHost.exe'
        assert native(host,{'op':'health'})['ok']
        assert native(host,{'op':'health'},origin='chrome-extension://'+'a'*32+'/')['code']=='BAD_ORIGIN'
        assert native(host,{},raw=struct.pack('<I',70000))['code']=='BAD_MESSAGE'
        assert native(host,{'op':'execute','command':'not executed'})['code']=='BAD_MESSAGE'
        added=native(host,{'op':'create','request':{**good,'request_id':str(uuid.uuid4()),'suggested_name':'native.bin'}})
        assert added['ok'],added
        wait_complete(port,added['task_id'])
        results.append('Native Messaging framing / origin / operation limits / actual download: PASS (SHA-256)')
        first.wait(timeout=15);assert first.returncode==0,first.stderr.read()
        log.close()
        assert not (DATA/'browser-endpoint.json').exists()
        second,newport,log2=start(1800)
        try:
            status,retried=request(newport,'POST','/api/v1/tasks',good)
            assert status==200 and retried['task_id']==created['task_id']
            with sqlite3.connect(DATA/'tasks.sqlite') as db:
                assert db.execute('PRAGMA user_version').fetchone()[0]==2
                assert db.execute("SELECT COUNT(*) FROM tasks WHERE id='legacy'").fetchone()[0]==1
                assert db.execute('SELECT COUNT(*) FROM tasks WHERE request_id=?',(good['request_id'],)).fetchone()[0]==1
            with sqlite3.connect(str(DATA/'tasks.sqlite')+'.pre-v2.bak') as backup:
                assert backup.execute('PRAGMA user_version').fetchone()[0]==1
                assert backup.execute("SELECT COUNT(*) FROM tasks WHERE id='legacy'").fetchone()[0]==1
            results.append('v1 migration preserves tasks; duplicate after restart remains one task: PASS')
            second.wait(timeout=5);assert second.returncode==0
        finally:
            if second.poll() is None:second.kill();second.wait()
            log2.close()
        (WORK/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
        print('\n'.join(results));print('Artifacts:',WORK)
    finally:
        if first.poll() is None:first.kill();first.wait()
        log.close()


if __name__=='__main__':
    try:main()
    finally:fixture.server.shutdown()

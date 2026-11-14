"""真实进程验证单实例、版本元信息及正常退出；不触及用户数据目录。"""
import json
import pathlib
import sqlite3
import subprocess
import time

import integration as fixture


def main():
    folder = fixture.WORK/'instance'
    folder.mkdir()
    data = folder/'data'
    command = [str(fixture.EXE), '--headless', '--url', fixture.BASE+'/slow',
               '--output', str(folder/'output.bin'), '--data-dir', str(data),
               '--connections', '4', '--pause-after', '2500']
    first = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        metadata = data/'runtime.json'
        deadline = time.monotonic()+10
        while not metadata.exists() and time.monotonic()<deadline:
            time.sleep(.05)
        info = json.loads(metadata.read_text(encoding='utf-8'))
        assert int(info['pid']) == first.pid
        expected=subprocess.run([str(fixture.EXE),'--headless','--version'],capture_output=True,timeout=5).stdout.decode().strip().split()[-1]
        assert info['version'] == expected
        assert pathlib.Path(info['executable']).resolve() == fixture.EXE.resolve()
        second = subprocess.run([str(fixture.EXE), '--headless', '--data-dir', str(data)],
                                capture_output=True, timeout=10)
        assert second.returncode == 3, second
        assert str(first.pid).encode() in second.stderr and expected.encode() in second.stderr
        assert first.poll() is None, 'The existing instance must not be terminated by a duplicate'
        stdout, stderr = first.communicate(timeout=15)
        assert first.returncode == 0, (stdout, stderr)
        assert not metadata.exists(), 'Normal exit must remove its runtime marker'
        with sqlite3.connect(data/'tasks.sqlite') as db:
            state, committed = db.execute('SELECT state,committed FROM tasks').fetchone()
        assert state == 'Paused' and committed > 0
        print('PASS: instance PID/version/path; duplicate rejected; first process preserved; paused checkpoint saved')
        print('Artifacts:', folder)
    finally:
        if first.poll() is None:
            first.kill()
            first.wait()


if __name__ == '__main__':
    try:
        main()
    finally:
        fixture.server.shutdown()

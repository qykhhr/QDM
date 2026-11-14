"""在独立、临时 Chrome profile 中加载真实扩展并验证页面嗅探→桌面下载。

仅自动化测试资料，使用 Chrome DevTools 协议。不注册用户浏览器的宿主，
不接触用户默认 profile；需要 websocket-client 和本机 Chrome。
"""
import base64
import argparse
import json
import pathlib
import subprocess
import threading
import time
import urllib.request
import websocket
import browser_bridge as harness

ROOT=pathlib.Path(__file__).resolve().parents[1]
ID=harness.ID
WORK=harness.WORK/'chrome'
WORK.mkdir()


class Client:
    def __init__(self,url):
        self.socket=websocket.create_connection(url,timeout=12,suppress_origin=True)
        self.counter=0
    def call(self,method,params=None,session=None):
        self.counter+=1;number=self.counter
        command={'id':number,'method':method,'params':params or {}}
        if session:command['sessionId']=session
        self.socket.send(json.dumps(command))
        while True:
            response=json.loads(self.socket.recv())
            if response.get('id')==number:
                if 'error' in response:raise RuntimeError((method,response['error']))
                return response.get('result',{})
    def evaluate(self,session,expression):
        result=self.call('Runtime.evaluate',{'expression':expression,'awaitPromise':True,'returnByValue':True},session)
        if result.get('exceptionDetails'):raise AssertionError(result['exceptionDetails'])
        return result['result'].get('value')


class PageHandler(harness.fixture.Handler):
    def do_GET(self):
        if self.path=='/favicon.ico':self.send_error(404);return
        if self.path.startswith('/index.html'):
            data=b'<html><head><title>QDM extension fixture</title></head><body><h1>QDM fixture</h1><img src="/pixel.png"><a href="/video.mp4">video file</a><a id="direct" href="/slow?file=auto-handoff.zip">Download directly</a><a id="fallback" href="/slow?file=fallback.zip">Download with unavailable desktop</a><script>fetch("/video.mp4").then(r=>r.arrayBuffer())</script></body></html>'
        elif self.path=='/pixel.png':
            data=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jEwAAAABJRU5ErkJggg==')
        else:return super().do_GET()
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.send_header('Content-Type','text/html' if self.path.split('?')[0].endswith('html') else 'image/png');self.end_headers();self.wfile.write(data)
    def end_headers(self):
        # 普通网页附件，真实浏览器会创建 downloads.onCreated 事件；不是扩展 API 下载。
        if self.path.startswith('/slow?file='):
            name=self.path.split('file=',1)[1]
            self.send_header('Content-Disposition',f'attachment; filename="{name}"')
            self.send_header('Content-Type','application/zip')
        super().end_headers()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--browser',choices=['chrome','edge'],default='chrome');args=parser.parse_args()
    desktop,port,log=harness.start(22000)
    page_server=harness.fixture.FixtureServer(('127.0.0.1',0),PageHandler)
    threading.Thread(target=page_server.serve_forever,daemon=True).start()
    profile=WORK/'profile'
    browser=r'C:\Program Files\Google\Chrome\Application\chrome.exe' if args.browser=='chrome' else r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
    chrome=subprocess.Popen([browser,'--headless=new','--no-first-run','--no-default-browser-check',
        '--enable-unsafe-extension-debugging','--remote-debugging-port=0',f'--user-data-dir={profile}',
        'about:blank'],stdout=subprocess.DEVNULL,stderr=(WORK/'chrome.log').open('wb'))
    client=None
    try:
        active=profile/'DevToolsActivePort';deadline=time.monotonic()+10
        while not active.exists() and time.monotonic()<deadline:time.sleep(.05)
        port_number,path=active.read_text().splitlines()[:2]
        client=Client(f'ws://127.0.0.1:{port_number}{path}')
        browser_files=WORK/'browser-downloads';browser_files.mkdir()
        client.call('Browser.setDownloadBehavior',{'behavior':'allow','downloadPath':str(browser_files),'eventsEnabled':True})
        targets=client.call('Target.getTargets')['targetInfos']
        assert client.call('Extensions.loadUnpacked',{'path':str(ROOT/'browser-extension')})['id']==ID
        (WORK/'extensions.json').write_text(json.dumps(client.call('Extensions.getExtensions'),indent=2))
        options=client.call('Target.createTarget',{'url':f'chrome-extension://{ID}/options.html'})['targetId']
        option_session=client.call('Target.attachToTarget',{'targetId':options,'flatten':True})['sessionId']
        client.call('Runtime.enable',session=option_session)
        deadline=time.monotonic()+5
        while not client.evaluate(option_session,f'document.readyState === "complete" && location.href === "chrome-extension://{ID}/options.html" && typeof chrome.runtime?.sendMessage === "function"') and time.monotonic()<deadline:time.sleep(.05)
        assert client.evaluate(option_session,'typeof chrome.runtime?.sendMessage === "function"'),client.evaluate(option_session,'({url:location.href,title:document.title,text:document.body.innerText})')
        pair={'endpoint':f'http://127.0.0.1:{port}','token':harness.token()}
        result=client.evaluate(option_session,f'chrome.runtime.sendMessage({json.dumps({"type":"saveConfig","mode":"portable","enabled":True,"pairing":json.dumps(pair)})})')
        assert result['ok'],result
        health=client.evaluate(option_session,'chrome.runtime.sendMessage({type:"health"})')
        assert health['ok'] and health['version']=='0.2.1',health
        page=f'http://127.0.0.1:{page_server.server_port}/index.html'
        target=client.call('Target.createTarget',{'url':page})['targetId']
        deadline=time.monotonic()+6;listed=None
        while time.monotonic()<deadline:
            listed=client.evaluate(option_session,f'(async()=>{{const tabs=await chrome.tabs.query({{url:{json.dumps(page)}}});if(!tabs.length)return null;const result=await chrome.runtime.sendMessage({{type:"list",tabId:tabs[0].id}});return {{...result,tabId:tabs[0].id}};}})()')
            if listed and any(x['kind']=='video' for x in listed.get('resources',[])):break
            time.sleep(.1)
        assert listed and listed['ok'],listed
        resource=next(x for x in listed['resources'] if x['kind']=='video')
        added=client.evaluate(option_session,f'chrome.runtime.sendMessage({json.dumps({"type":"send","tabId":listed["tabId"],"id":resource["id"],"page":page})})')
        assert added['ok'],added
        result=harness.wait_complete(port,added['task_id'])
        # 点击网页原始下载链接，不能再通过扩展列表里的“下载”按钮发起。
        page_session=client.call('Target.attachToTarget',{'targetId':target,'flatten':True})['sessionId']
        client.call('Runtime.evaluate',{'expression':'document.getElementById("direct").click()','userGesture':True},page_session)
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            state=client.evaluate(option_session,'chrome.downloads.search({}).then(items=>items.filter(x=>x.url.includes("file=auto-handoff.zip")).map(x=>({id:x.id,state:x.state,paused:x.paused,error:x.error,byExtensionId:x.byExtensionId})))')
            if state and state[0]['state']=='interrupted':break
            time.sleep(.1)
        assert state and state[0]['state']=='interrupted',state
        assert not state[0].get('byExtensionId'),state
        import sqlite3
        with sqlite3.connect(harness.DATA/'tasks.sqlite') as db:
            automatic=db.execute("SELECT id,path FROM tasks WHERE path LIKE '%auto-handoff.zip'").fetchone()
        assert automatic,'Original webpage download was not handed to QDM'
        harness.wait_complete(port,automatic[0])
        # 交接失败时，浏览器下载应恢复，并且桌面端不增加重复/失败任务。
        invalid_pair={**pair,'token':'0'*64}
        config={'type':'saveConfig','mode':'portable','enabled':True,'takeover':True,'pairing':json.dumps(invalid_pair)}
        assert client.evaluate(option_session,f'chrome.runtime.sendMessage({json.dumps(config)})')['ok']
        # 第二个独立场景使用新的网页文档，避免浏览器“同页多次下载”策略干扰回退测试。
        fallback_target=client.call('Target.createTarget',{'url':page+'?fallback-case=1'})['targetId']
        fallback_session=client.call('Target.attachToTarget',{'targetId':fallback_target,'flatten':True})['sessionId']
        deadline=time.monotonic()+5
        while not client.evaluate(fallback_session,'Boolean(document.getElementById("fallback"))') and time.monotonic()<deadline:time.sleep(.05)
        clicked=client.call('Runtime.evaluate',{'expression':'document.getElementById("fallback").click()','userGesture':True},fallback_session)
        assert not clicked.get('exceptionDetails'),clicked
        deadline=time.monotonic()+6
        while time.monotonic()<deadline:
            fallback=client.evaluate(option_session,'(async()=>({items:(await chrome.downloads.search({})).filter(x=>x.url.includes("file=fallback.zip")).map(x=>({state:x.state,paused:x.paused})),error:(await chrome.storage.session.get("lastError")).lastError}))()')
            if fallback['items'] and not fallback['items'][0]['paused'] and '已恢复' in (fallback.get('error') or ''):break
            time.sleep(.1)
        assert fallback['items'] and not fallback['items'][0]['paused'] and '已恢复' in fallback.get('error',''),fallback
        with sqlite3.connect(harness.DATA/'tasks.sqlite') as db:assert db.execute("SELECT COUNT(*) FROM tasks WHERE path LIKE '%fallback.zip'").fetchone()[0]==0
        await_config={'type':'saveConfig','mode':'portable','enabled':True,'takeover':True,'pairing':json.dumps(pair)}
        client.evaluate(option_session,f'chrome.runtime.sendMessage({json.dumps(await_config)})')
        # 独立扩展页面截图检查样式；background 创建避免改变当前网页标签。
        popup=client.call('Target.createTarget',{'url':f'chrome-extension://{ID}/popup.html','background':True})['targetId']
        popup_session=client.call('Target.attachToTarget',{'targetId':popup,'flatten':True})['sessionId']
        client.call('Emulation.setDeviceMetricsOverride',{'width':460,'height':650,'deviceScaleFactor':1,'mobile':False},popup_session)
        time.sleep(.5)
        screenshot=client.call('Page.captureScreenshot',{'format':'png'},popup_session)
        (WORK/'popup.png').write_bytes(base64.b64decode(screenshot['data']))
        record={'browser':args.browser+' temporary profile','extension_id':ID,'health_version':health['version'],'resource_count':len(listed['resources']),'task_state':result['state'],'sha256_verified':True,'webpage_download_taken_over':True,'browser_download_cancelled_after_accept':True,'fallback_browser_resumed':True}
        (WORK/'results.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
        print(json.dumps(record));print('Artifacts:',WORK)
    finally:
        if client:
            try:client.call('Browser.close')
            except Exception:pass
            client.socket.close()
        try:chrome.wait(timeout=5)
        except subprocess.TimeoutExpired:chrome.terminate();chrome.wait(timeout=5)
        page_server.shutdown()
        desktop.wait(timeout=25);log.close()


if __name__=='__main__':
    try:main()
    finally:harness.fixture.server.shutdown()

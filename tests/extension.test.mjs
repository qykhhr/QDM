import test from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto, createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { httpUrl, suggestedName, resourceKind, fromHeaders, parsePairing, trimResources } from '../browser-extension/model.js';
const ID = 'nkblcdnjneadmggjpnjajlbebpbmhjpj';
test('manifest has stable ID, MV3 and download handoff permissions', async () => {
  const manifest = JSON.parse(await readFile(new URL('../browser-extension/manifest.json', import.meta.url)));
  const id = [...createHash('sha256').update(Buffer.from(manifest.key, 'base64')).digest('hex').slice(0,32)].map(n => String.fromCharCode(97+parseInt(n,16))).join('');
  assert.equal(id, ID); assert.equal(manifest.manifest_version, 3);
  assert(manifest.permissions.includes('downloads')); assert(manifest.permissions.includes('alarms'));
  assert(!manifest.permissions.includes('webRequestBlocking')); assert(!manifest.permissions.includes('cookies'));
  assert(!manifest.externally_connectable); assert(!manifest.content_scripts);
});
test('HTTP URL validation rejects credentials, special protocols and preserves signed query', () => {
  assert.equal(httpUrl('https://example.org/a.zip?Signature=ab~c__&x=1#top'), 'https://example.org/a.zip?Signature=ab~c__&x=1');
  for (const u of ['javascript:alert(1)', 'file:///x', 'blob:https://a/x', 'https://u:p@example.org/x']) assert.equal(httpUrl(u), null);
});
test('filename handling supports UTF-8 disposition and strips traversal/reserved names', () => {
  assert.equal(suggestedName('https://a/download', "attachment; filename*=UTF-8''%E6%B5%8B%E8%AF%95.zip"), '测试.zip');
  assert.equal(suggestedName('https://a/download', 'attachment; filename="../../CON.exe"'), '_CON.exe');
  assert.equal(suggestedName('https://a/download', "filename*=UTF-8''%ZZ"), 'download.bin');
});
test('classify streams separately and avoid flooding lists with media fragments', () => {
  assert.equal(resourceKind('https://a/movie.m3u8?token=x'), 'stream');
  assert.equal(resourceKind('https://a/media', 'application/dash+xml'), 'stream');
  assert.equal(resourceKind('https://a/segment.ts', 'video/mp2t'), null);
  assert.equal(resourceKind('https://a/installer.exe'), 'program');
  assert.equal(resourceKind('https://a/api', 'application/json'), null);
});
test('network candidates only include successful tab requests; ranges use full length', () => {
  const input = { tabId: 1, statusCode: 206, url: 'https://a/movie.mp4', responseHeaders: [{ name:'Content-Range',value:'bytes 0-99/2000' },{name:'Content-Length',value:'100'}] };
  assert.equal(fromHeaders(input).size, 2000); assert.equal(fromHeaders({...input,tabId:-1}), null); assert.equal(fromHeaders({...input,statusCode:403}), null);
});
test('pairing only accepts a fixed IPv4 loopback endpoint and a strong token', () => {
  const token='a'.repeat(64); assert.equal(parsePairing({endpoint:'http://127.0.0.1:17878',token}).endpoint, 'http://127.0.0.1:17878');
  for(const endpoint of ['https://127.0.0.1:17878','http://evil.test:17878','http://127.0.0.1:17878/path','http://user@127.0.0.1:17878']) assert.throws(()=>parsePairing({endpoint,token}));
});
test('resource state is bounded across tabs', () => {
  const store={}; for(let t=0;t<10;t++)store[t]=Array.from({length:130},(_,i)=>({id:`${t}-${i}`,seen:t*1000+i}));
  trimResources(store);assert.equal(Object.values(store).flat().length,400);assert(Object.values(store).every(items=>items.length<=100));
});
test('worker preserves detection over a restart and enforces trusted message senders', async () => {
  if(!globalThis.crypto)globalThis.crypto=webcrypto;
  const event=()=>({listeners:[],addListener(fn){this.listeners.push(fn);}});
  const local={},session={};const area=data=>({async get(keys){if(typeof keys==='string')return {[keys]:data[keys]};return Object.fromEntries(keys.map(k=>[k,data[k]]));},async set(value){Object.assign(data,structuredClone(value));},async remove(key){delete data[key];}});
  globalThis.chrome={storage:{local:area(local),session:area(session)},runtime:{id:ID,getURL:path=>`chrome-extension://${ID}/${path}`,onMessage:event(),onInstalled:event(),onStartup:event(),sendNativeMessage(host,payload,callback){callback({ok:true,task_id:'task-test',version:'0.2.0',connections:4});}},
    webRequest:{onBeforeRequest:event(),onHeadersReceived:event()},tabs:{onRemoved:event()},action:{async setBadgeText(){},async setBadgeBackgroundColor(){}},
    contextMenus:{onClicked:event(),removeAll(fn){fn();},create(){}}};
  chrome.downloads={onCreated:event(),async search(){return [];}};
  chrome.alarms={create(){},onAlarm:event()};
  const worker=new URL('../browser-extension/background.js',import.meta.url);await import(worker.href+'?first');
  const send=(m,sender={id:ID,url:`chrome-extension://${ID}/popup.html`})=>new Promise(resolve=>chrome.runtime.onMessage.listeners.at(-1)(m,sender,resolve));
  for(let i=0;i<12;i++)chrome.webRequest.onHeadersReceived.listeners[0]({tabId:3,statusCode:200,url:`https://a/${i}.zip`,responseHeaders:[]});
  let listed=await send({type:'list',tabId:3});assert.equal(listed.resources.length,12);
  assert.equal((await send({type:'health'},{id:ID,tab:{id:3}})).ok,false);
  assert.equal((await send({type:'health'},{id:ID,url:`chrome-extension://${ID}/options.html`,tab:{id:7}})).ok,true);
  const id=listed.resources[0].id;assert.equal((await send({type:'send',tabId:3,id})).task_id,'task-test');
  listed=await send({type:'list',tabId:3});assert.equal(listed.resources[0].taskId,'task-test');
  // 模拟 worker 被销毁：监听器消失，但 storage.session 仍然保留。
  for(const domain of Object.values(chrome))for(const value of Object.values(domain))if(Array.isArray(value?.listeners))value.listeners.length=0;
  await import(worker.href+'?restart');listed=await send({type:'list',tabId:3});assert.equal(listed.resources.length,12);
  for(const listener of chrome.webRequest.onBeforeRequest.listeners)listener({tabId:3,type:'main_frame',method:'GET'});
  listed=await send({type:'list',tabId:3});assert.equal(listed.resources.length,0);
});

/** 用假浏览器 API 验证交接顺序和故障恢复，不操作用户真实下载。 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { DownloadTakeover } from '../browser-extension/takeover.js';
function fixture(overrides={}) {
  let saved={};const calls=[];const item={id:3,url:'https://files.example/a.zip',state:'in_progress',paused:false,danger:'safe',filename:'C:\\Downloads\\a.zip'};
  const services={storage:{async get(){return structuredClone(saved);},async set(value){saved={...saved,...structuredClone(value)};}},
    downloads:{async search(){return [{...item}];},async pause(){calls.push('pause');item.paused=true;},async cancel(){calls.push('cancel');item.state='interrupted';},async resume(){calls.push('resume');item.paused=false;}},
    getConfig:async()=>({takeover:true}),isPost:async()=>false,
    createInQdm:async resource=>{calls.push('create');return {task_id:'task-1',requestId:resource.id};},onError:async text=>calls.push('error'),...overrides};
  return {flow:new DownloadTakeover(services),item,calls,services,saved:()=>saved};
}
test('交接成功：先暂停，桌面确认后才取消浏览器，并清理日志',async()=>{
  const f=fixture();await f.flow.handle(f.item);assert.deepEqual(f.calls,['pause','create','cancel']);assert.deepEqual(f.saved().handoffs,{});
});
test('桌面不可用：恢复浏览器，绝不取消原下载',async()=>{
  const f=fixture({createInQdm:async()=>{throw new Error('offline');}});await f.flow.handle(f.item);assert.deepEqual(f.calls,['pause','resume','error']);assert.deepEqual(f.saved().handoffs,{});
});
test('用户关闭自动接管时不暂停，也不创建桌面任务',async()=>{
  const f=fixture({getConfig:async()=>({takeover:false})});await f.flow.handle(f.item);assert.deepEqual(f.calls,[]);
});
test('非HTTP、隐私、其他扩展、不安全、已暂停、已知POST保留在浏览器',async()=>{
  for(const change of [{url:'blob:https://files.example/x'},{incognito:true},{byExtensionId:'other'},{danger:'url'},{paused:true},{state:'complete'}]){
    const f=fixture();Object.assign(f.item,change);await f.flow.handle(f.item);assert.deepEqual(f.calls,[]);
  }
  const f=fixture({isPost:async()=>true});await f.flow.handle(f.item);assert.deepEqual(f.calls,[]);
});
test('重启时已有桌面确认：只补取消，不重复发送',async()=>{
  const f=fixture();f.item.paused=true;await f.flow.journal(3,{phase:'accepted',taskId:'saved',requestId:'same'});await f.flow.recover();assert.deepEqual(f.calls,['cancel']);
});
test('重启时尚未确认：复用原幂等键继续交接',async()=>{
  let request;const f=fixture({createInQdm:async r=>{request=r.id;return {task_id:'saved'};}});f.item.paused=true;
  await f.flow.journal(3,{phase:'paused',requestId:'persisted-request'});await f.flow.recover();assert.equal(request,'persisted-request');assert.deepEqual(f.calls,['cancel']);
});
test('取消失败：保留 accepted 日志，下一次恢复只重试取消',async()=>{
  const f=fixture();f.services.downloads.cancel=async()=>{throw new Error('transient');};await f.flow.handle(f.item);
  assert.equal(f.saved().handoffs['3'].phase,'accepted');assert(!f.calls.includes('resume'));
  f.services.downloads.cancel=async()=>f.calls.push('cancel');await f.flow.recover();assert.equal(f.calls.filter(x=>x==='create').length,1);assert.deepEqual(f.saved().handoffs,{});
});
test('浏览器拒绝暂停：不能提前创建桌面任务',async()=>{
  const f=fixture();f.services.downloads.pause=async()=>{throw new Error('already complete');};await f.flow.handle(f.item);assert(!f.calls.includes('create'));assert(!f.calls.includes('cancel'));
});

/**
 * MV3 service worker。监听器同步注册；页面资源只存 session，不上传历史。
 * 所有 session 读改写串行，防止高并发请求和 worker 重启丢失/覆盖候选资源。
 */
import {
    HOST,
    httpUrl,
    suggestedName,
    fromHeaders,
    resourceKind,
    parsePairing,
    trimResources
} from './model.js';
import {DownloadTakeover} from './takeover.js';
let queue = Promise.resolve();
/**
 * 把 session 读改写串行化。前一个操作失败只影响自己的调用者，不会让后续所有事件永远挂在 rejected
 * Promise 上。
 */
function serial(work) {
    const next = queue.then(work);
    queue = next.catch(() => {});
    return next;
}
const defaults = {
    enabled : true,
    takeover : true,
    mode : 'native'
};
/**
 * 合并默认值和本机设置。旧配置没有 takeover 字段时按当前默认开启，资源识别和下载接管可以分别关闭。
 */
async function config() {
    const {config : saved} = await chrome.storage.local.get('config');
    return {...defaults, ...saved};
}
/**
 * 显示当前标签资源数量。标签可能已经关闭，UI 更新失败不应中断候选保存或下载交接。
 */
async function badge(tabId, count) {
    await chrome.action.setBadgeBackgroundColor({color : '#126b50', tabId}).catch(() => {});
    await chrome.action.setBadgeText({text : count ? String(count) : '', tabId}).catch(() => {});
}
/**
 * 在已取得串行执行权后更新资源。按完整 URL 去重，保留已发送任务
 * ID；写入后更新徽标。不得从队列内再次等待 remember，否则会自锁。
 */
async function rememberNow(tabId, resource) {
    const {resources = {}} = await chrome.storage.session.get('resources');
    const items = resources[tabId] || [];
    const existing = items.find(x => x.url === resource.url);
    if (existing)
        Object.assign(existing, resource, {seen : Date.now()});
    else
        items.push({...resource, id : crypto.randomUUID(), seen : Date.now()});
    resources[tabId] = items;
    trimResources(resources);
    await chrome.storage.session.set({resources});
    await badge(tabId, resources[tabId]?.length || 0);
}
/**
 * 资源更新的队列入口。外部调用必须使用此函数，只有已经在 serial 内的网络监听器才能调用
 * rememberNow。
 */
function remember(tabId, resource) {
    return serial(() => rememberNow(tabId, resource));
}
/**
 * 按导航/关闭清除本标签的会话候选。与网络写入共享同一个队列，防止较慢的配置读取跨过清空动作。
 */
function clearTab(tabId) {
    return serial(async () => {
        const {resources = {}} = await chrome.storage.session.get('resources');
        delete resources[tabId];
        await chrome.storage.session.set({resources});
        await badge(tabId, 0);
    });
}
chrome.webRequest.onBeforeRequest.addListener(details => {
    if (details.tabId >= 0)
        void clearTab(details.tabId).catch(() => {});
}, {urls : [ 'http://*/*', 'https://*/*' ], types : [ 'main_frame' ]});
// downloads API 不提供原请求方法。记录近期 POST 的 URL，避免把表单下载误改为 GET。
// 只保存 100 条 URL/时间，不读取或记录 POST 正文、Cookie、Authorization。
chrome.webRequest.onBeforeRequest.addListener(details => {
    if (details.method !== 'POST')
        return;
    void serial(async () => {
        const {recentPosts = []} = await chrome.storage.session.get('recentPosts');
        const next = recentPosts.filter(x => Date.now() - x.seen < 60000);
        next.push({url : httpUrl(details.url), seen : Date.now()});
        await chrome.storage.session.set({recentPosts : next.slice(-100)});
    }).catch(() => {});
}, {urls : [ 'http://*/*', 'https://*/*' ]});
chrome.webRequest.onHeadersReceived.addListener(details => {
    // 本机控制接口不会进入资源列表；没有阻塞/修改页面请求。
    if (details.tabId < 0)
        return;
    const item = fromHeaders(details);
    if (!item)
        return;
    // 先入串行队列再读配置，保证导航清空、嗅探和 popup 读取的先后次序。
    void serial(async () => {
        const c = await config();
        if (c.enabled)
            await rememberNow(details.tabId, item);
    }).catch(() => {});
}, {urls : [ 'http://*/*', 'https://*/*' ]}, [ 'responseHeaders' ]);
chrome.tabs.onRemoved.addListener(tabId => { void clearTab(tabId).catch(() => {}); });
/**
 * 重新建立固定右键菜单。先清旧项，避免更新或重启后出现重复菜单；不接管任何浏览器文件删除行为。
 */
function installMenu() {
    chrome.contextMenus.removeAll(() => {
        void chrome.runtime.lastError;
        chrome.contextMenus.create({
            id : 'qdm-download',
            title : '使用 QDM 下载',
            contexts : [ 'link', 'image', 'video', 'audio' ]
        },
                                   () => { void chrome.runtime.lastError; });
    });
}
chrome.runtime.onInstalled.addListener(installMenu);
chrome.runtime.onStartup.addListener(installMenu);

/** 自动接管与嗅探分离：关闭资源列表扫描不会悄悄关闭用户要求的下载接管。 */
const takeover = new DownloadTakeover({
    downloads : chrome.downloads,
    storage : chrome.storage.local,
    getConfig : config,
    createInQdm : async (resource, page) => {
        // onCreated 时浏览器文件名可能尚未确定，优先使用已观察到的响应文件名。
        if (!resource.name) {
            await queue;
            const {resources = {}} = await chrome.storage.session.get('resources');
            const known = Object.values(resources).flat().find(x => x.url === resource.url);
            resource = {...resource, name : known?.name || suggestedName(resource.url)};
        }
        return create(resource, page);
    },
    isPost : async url => {
        await queue;
        const {recentPosts = []} = await chrome.storage.session.get('recentPosts');
        return recentPosts.some(x => x.url === url && Date.now() - x.seen < 60000);
    },
    onSuccess : async () => { await chrome.storage.session.remove('lastError'); },
    onError : async message => { await chrome.storage.session.set({lastError : message}); }
});
if (chrome.downloads?.onCreated && chrome.alarms?.onAlarm) {
    chrome.downloads.onCreated.addListener(item => { void takeover.handle(item); });
    chrome.runtime.onStartup.addListener(() => { void takeover.recover().catch(() => {}); });
    // worker 可能因空闲回收而重建，未必伴随整个浏览器 onStartup。
    void takeover.recover().catch(() => {});
    // 交接确认后取消失败不能恢复成双份下载，每分钟唤醒重试残留日志。
    chrome.alarms.create('qdm-recover-handoffs', {periodInMinutes : 1});
    chrome.alarms.onAlarm.addListener(alarm => {
        if (alarm.name === 'qdm-recover-handoffs')
            void takeover.recover().catch(() => {});
    });
} else {
    // 本地源码已更新但浏览器尚未重新加载旧 manifest 时，不能让整个 worker 崩溃。
    void chrome.storage.session.set({lastError:'请在扩展管理页重新加载 QDM 扩展并允许“管理下载”权限，自动接管才能生效。'});
}

/**
 * 在 Native Messaging 与便携 HTTP 之间适配同一业务消息。便携请求有超时、禁止凭据 Cookie
 * 和重定向，防止把配对密钥送到其他地址。
 */
async function desktop(message) {
    const c = await config();
    let result;
    if (c.mode === 'portable') {
        const pair = parsePairing(c.pairing || {});
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 12000);
        try {
            let path = '/api/v1/health', method = 'GET';
            if (message.op === 'create') {
                path = '/api/v1/tasks';
                method = 'POST';
            } else if (message.op === 'status')
                path = '/api/v1/tasks/' + encodeURIComponent(message.task_id);
            const response = await fetch(pair.endpoint + path, {
                method,
                signal : controller.signal,
                cache : 'no-store',
                credentials : 'omit',
                redirect : 'error',
                headers : {
                    Authorization : 'Bearer ' + pair.token,
                    'Content-Type' : 'application/json',
                    'X-QDM-Extension-Id' : chrome.runtime.id
                },
                ...(method === 'POST' ? {body : JSON.stringify(message.request)} : {})
            });
            result = await response.json();
        } finally {
            clearTimeout(timeout);
        }
    } else {
        result = await new Promise(
            (resolve, reject) => chrome.runtime.sendNativeMessage(HOST, message, reply => {
                const failure = chrome.runtime.lastError;
                if (failure)
                    reject(new Error(
                        '未连接本机宿主。请先启动 QDM，并按安装说明注册宿主，或切换为便携配对。'));
                else
                    resolve(reply);
            }));
    }
    if (!result || !result.ok)
        throw new Error(result?.message || '桌面程序未响应');
    return result;
}
/**
 * 构造桌面受限下载请求。request_id 在重试间保持稳定；只有桌面返回持久化确认才成功。不能把 HLS/DASH
 * 清单当作已合并视频。
 */
async function create(resource, sourcePage = '') {
    const url = httpUrl(resource.url);
    if (!url)
        throw new Error('仅支持 HTTP/HTTPS 地址');
    if (resource.kind === 'stream' || /\.(m3u8|mpd)$/i.test(new URL(url).pathname))
        throw new Error('流媒体清单已识别，当前桌面版尚未支持合并下载');
    const result = await desktop({
        op : 'create',
        request : {
            request_id : resource.id || crypto.randomUUID(),
            url,
            suggested_name : resource.name || suggestedName(url),
            profile_id : 'default',
            source_page : httpUrl(sourcePage) || ''
        }
    });
    await chrome.storage.session.remove('lastError');
    return result;
}
chrome.contextMenus.onClicked.addListener((info, tab) => {
    if (info.menuItemId !== 'qdm-download')
        return;
    const url = httpUrl(info.linkUrl || info.srcUrl);
    if (!url)
        return;
    void create({url, kind : resourceKind(url), name : suggestedName(url)}, tab?.url)
        .then(() => {
            if (tab?.id >= 0)
                void chrome.action.setBadgeText({text : '✓', tabId : tab.id});
        })
        .catch(async error => {
            await chrome.storage.session.set({lastError : error.message});
            if (tab?.id >= 0) {
                await chrome.action.setBadgeBackgroundColor({color : '#b54032', tabId : tab.id});
                await chrome.action.setBadgeText({text : '!', tabId : tab.id});
            }
        });
});
chrome.runtime.onMessage.addListener((message, sender, respond) => {
    // 没有 externally_connectable；再拒绝内容脚本/网页消息，只有本扩展页面能控制下载。
    const trusted = sender.id === chrome.runtime.id && [
        chrome.runtime.getURL('popup.html'), chrome.runtime.getURL('options.html')
    ].includes(sender.url);
    if (!trusted) {
        respond({ok : false, error : '不允许的消息来源'});
        return false;
    }
    const action = async () => {
        if (message.type === 'list') {
            await queue;
            const {resources = {}, lastError} =
                await chrome.storage.session.get([ 'resources', 'lastError' ]);
            return {
                resources : resources[message.tabId] || [],
                lastError,
                config : {enabled : (await config()).enabled}
            };
        }
        if (message.type === 'health')
            return desktop({op : 'health'});
        if (message.type === 'send') {
            await queue;
            const {resources = {}} = await chrome.storage.session.get('resources');
            const resource = (resources[message.tabId] || []).find(x => x.id === message.id);
            if (!resource)
                throw new Error('资源已随页面切换清除，请重新扫描');
            const result = await create(resource, message.page);
            await remember(message.tabId, {...resource, taskId : result.task_id});
            return result;
        }
        if (message.type === 'manual')
            return create({url : message.url, id : message.requestId});
        if (message.type === 'clear') {
            await clearTab(message.tabId);
            await chrome.storage.session.remove('lastError');
            return {};
        }
        if (message.type === 'scan') {
            const results = await chrome.scripting.executeScript(
                {target : {tabId : message.tabId}, files : [ 'scan.js' ]});
            for (const item of results.flatMap(r => r.result || [])) {
                const url = httpUrl(item.url);
                const kind = url && resourceKind(url, item.mime);
                if (kind)
                    await remember(message.tabId, {
                        url,
                        kind,
                        name : suggestedName(url),
                        size : null,
                        mime : item.mime || ''
                    });
            }
            return {};
        }
        if (message.type === 'saveConfig') {
            const previous = await config();
            const next = {
                enabled : message.enabled !== false,
                takeover : message.takeover ?? previous.takeover,
                mode : message.mode === 'portable' ? 'portable' : 'native'
            };
            if (next.mode === 'portable')
                next.pairing = parsePairing(message.pairing);
            await chrome.storage.local.set({config : next});
            return {};
        }
        if (message.type === 'getConfig')
            return {config : await config(), extensionId : chrome.runtime.id};
        throw new Error('未知操作');
    };
    action().then(data => respond({ok : true, ...data}),
                  error => respond({ok : false, error : error.message || '操作失败'}));
    return true;
});

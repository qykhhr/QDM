/** popup 不直接读网页或本机接口；通过 worker 的受限消息契约执行。 */
const $ = id => document.getElementById(id);
let tab = null, items = [], filter = 'all', manualRequest = null;
const labels = {
    video : '视频',
    audio : '音频',
    image : '图片',
    program : '程序',
    archive : '压缩包',
    document : '文档',
    stream : '流媒体清单',
    file : '文件'
};
/**
 * 向本扩展 worker 发消息并统一传播错误。弹窗不直接读取网页、访问数据库或操作本机下载目录。
 */
async function call(message) {
    const result = await chrome.runtime.sendMessage(message);
    if (!result?.ok)
        throw new Error(result?.error || '扩展暂未就绪');
    return result;
}
/**
 * 用纯文本显示操作结果，不将资源文件名或远程错误解释为 HTML。
 */
function message(text, bad = false) {
    $('message').textContent = text;
    $('message').classList.toggle('error', bad);
}
/**
 * 格式化候选资源的估计大小。未知保持未知，不能用 0 冒充尚未探测的长度。
 */
function size(bytes) {
    if (bytes === null || bytes === undefined)
        return '大小未知';
    if (bytes < 1024)
        return bytes + ' B';
    if (bytes < 1048576)
        return (bytes / 1024).toFixed(1) + ' KiB';
    return (bytes / 1048576).toFixed(1) + ' MiB';
}
/**
 * 重建当前筛选后的候选列表。DOM 一律通过 textContent 写入；流媒体清单禁用普通下载，已提交资源保留
 * taskId 防止重复点击。
 */
function render() {
    $('count').textContent = items.length;
    const container = $('resources');
    container.replaceChildren();
    const visible = items.filter(
        x => filter === 'all' || x.kind === filter ||
             (filter === 'files' && [ 'program', 'archive', 'document', 'file' ].includes(x.kind)));
    if (!visible.length) {
        const empty = document.createElement('p');
        empty.className = 'empty';
        empty.textContent = '尚未发现此类资源。刷新网页或播放音视频后再查看，也可点击扫描页面。';
        container.append(empty);
        return;
    }
    for (const item of visible.slice().reverse()) {
        const row = document.createElement('article');
        const body = document.createElement('div');
        body.className = 'resource';
        const title = document.createElement('strong');
        title.textContent = item.name;
        title.title = item.url;
        const info = document.createElement('p');
        info.textContent =
            `${labels[item.kind]} · ${size(item.size)} · ${new URL(item.url).hostname}`;
        body.append(title, info);
        const actions = document.createElement('div');
        actions.className = 'row-actions';
        const copy = document.createElement('button');
        copy.className = 'quiet';
        copy.textContent = '复制';
        copy.onclick = () => navigator.clipboard.writeText(item.url).then(
            () => message('已复制资源地址'),
            () => message('复制失败，请展开浏览器的剪贴板权限', true));
        const download = document.createElement('button');
        download.textContent = item.taskId ? '已添加' : '下载';
        download.disabled = !!item.taskId || item.kind === 'stream';
        download.title = item.kind === 'stream' ? '当前桌面版尚未支持流媒体合并，可先复制清单地址'
                                                : '发送到 QDM';
        download.onclick = async () => {
            download.disabled = true;
            try {
                await call({type : 'send', tabId : tab.id, id : item.id, page : tab.url});
                message('任务已交给 QDM，保存位置由桌面程序设置');
                await reload();
            } catch (error) {
                message(error.message, true);
                download.disabled = false;
            }
        };
        actions.append(copy, download);
        row.append(body, actions);
        container.append(row);
    }
}
/**
 * 从 worker 读取当前标签的持久化会话快照。即使 service worker 刚重启，弹窗也不依赖旧进程内存。
 */
async function reload() {
    const state = await call({type : 'list', tabId : tab?.id ?? -1});
    items = state.resources;
    render();
    if (state.lastError)
        message(state.lastError, true);
}
$('settings').onclick = () => chrome.runtime.openOptionsPage();
$('scan').onclick = async () => {
    try {
        await call({type : 'scan', tabId : tab.id});
        await reload();
        message('已扫描当前页面');
    } catch (error) {
        message('无法扫描此页面：' + error.message, true);
    }
};
$('clear').onclick = async () => {
    await call({type : 'clear', tabId : tab?.id ?? -1});
    message('已清空当前页面资源');
    await reload();
};
for (const button of document.querySelectorAll('[data-kind]'))
    button.onclick = () => {
        filter = button.dataset.kind;
        for (const b of document.querySelectorAll('[data-kind]'))
            b.classList.toggle('selected', b === button);
        render();
    };
$('url').oninput = () => { manualRequest = null; };
$('manual').onsubmit = async event => {
    event.preventDefault();
    const button = event.submitter || $('manual').querySelector('button');
    button.disabled = true;
    manualRequest ||= crypto.randomUUID();
    try {
        await call({type : 'manual', url : $('url').value.trim(), requestId : manualRequest});
        message('任务已添加到 QDM');
        $('url').value = '';
        manualRequest = null;
    } catch (error) {
        message(error.message, true);
    } finally {
        button.disabled = false;
    }
};
try {
    [tab] = await chrome.tabs.query({active : true, currentWindow : true});
    $('scan').disabled = !tab || !/^https?:/.test(tab.url || '');
    await reload();
} catch (error) {
    message(error.message, true);
}
call({type : 'health'})
    .then(
        health => {
            $('connection').textContent = `已连接 QDM ${health.version}`;
            $('dot').classList.add('connected');
        },
        error => {
            $('connection').textContent = '桌面未连接';
            message(error.message, true);
        });

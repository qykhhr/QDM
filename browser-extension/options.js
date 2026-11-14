const $ = id => document.getElementById(id);
/**
 * 通过受信扩展页面消息修改配置；桌面通信仍统一由 worker 校验和执行。
 */
async function call(message) {
    const result = await chrome.runtime.sendMessage(message);
    if (!result?.ok)
        throw new Error(result?.error || '扩展暂未就绪');
    return result;
}
/**
 * 根据连接模式显示相应的帮助和配对输入，不清除用户尚未提交的编辑内容。
 */
function mode() {
    $('pairing-field').hidden = $('mode').value !== 'portable';
    $('native-help').hidden = $('mode').value === 'portable';
}
/**
 * 展示保存/连接结果，错误和成功用同一个纯文本区域，避免注入页面内容。
 */
function show(text, bad = false) {
    $('result').textContent = text;
    $('result').classList.toggle('error', bad);
}
/**
 * 收集资源识别、自动接管及连接方式。Token 只保存在本机 storage.local，不进入账号同步或网页 DOM。
 */
async function save() {
    return call({
        type : 'saveConfig',
        mode : $('mode').value,
        enabled : $('enabled').checked,
        takeover : $('takeover').checked,
        pairing : $('pairing').value
    });
}
$('mode').onchange = mode;
$('config').onsubmit = async event => {
    event.preventDefault();
    try {
        await save();
        show('设置已保存');
    } catch (error) {
        show(error.message, true);
    }
};
$('test').onclick = async () => {
    try {
        await save();
        const result = await call({type : 'health'});
        show(`已连接 QDM ${result.version}，默认并发上限 ${result.connections} 路`);
    } catch (error) {
        show(error.message, true);
    }
};
try {
    const {config, extensionId} = await call({type : 'getConfig'});
    $('enabled').checked = config.enabled;
    $('takeover').checked = config.takeover;
    $('mode').value = config.mode;
    $('extension-id').textContent = extensionId;
    $('pairing').value = config.pairing ? JSON.stringify(config.pairing) : '';
    mode();
} catch (error) {
    show(error.message, true);
}

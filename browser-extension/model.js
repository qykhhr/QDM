/** 纯数据逻辑；没有浏览器 API，供 service worker 与独立测试共用。 */
export const HOST = 'com.qdm.download_manager';
export const MAX_PER_TAB = 100;
export const MAX_TOTAL = 400;
/**
 * 验证并规范化可下载地址。只接受 HTTP(S)，拒绝 URL 内嵌账号密码；只移除
 * fragment，保留签名查询参数原样。非法输入返回 null。
 */
export function httpUrl(value) {
    try {
        const u = new URL(value);
        if (!['http:', 'https:'].includes(u.protocol) || u.username || u.password ||
            u.href.length > 8192)
            return null;
        u.hash = '';
        return u.href;
    } catch {
        return null;
    }
}
/**
 * 按 RFC 5987 UTF-8 文件名、普通 Content-Disposition、URL
 * 路径的顺序推断名称。名称始终作为纯文本，并清理路径穿越/Windows 设备名；真正目录由桌面再次限制。
 */
export function suggestedName(url, disposition = '') {
    let name = '';
    const encoded = /filename\*\s*=\s*UTF-8''([^;]+)/i.exec(disposition);
    const plain = /filename\s*=\s*(?:"([^"]*)"|([^;]+))/i.exec(disposition);
    try {
        name = encoded ? decodeURIComponent(encoded[1])
               : plain ? (plain[1] ?? plain[2])
                       : decodeURIComponent(new URL(url).pathname.split('/').pop());
    } catch {
        name = '';
    }
    name = name.split(/[\\/]/)
               .pop()
               .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
               .replace(/[ .]+$/g, '')
               .trim();
    if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name))
        name = '_' + name;
    return (name || 'download.bin').slice(0, 140);
}
/**
 * 根据 URL、MIME 和响应附件标记分类。流媒体清单单独返回 stream；排除 ts/m4s
 * 分片，避免候选列表被分片占满。没有可下载特征时返回 null。
 */
export function resourceKind(url, mime = '', disposition = '') {
    const valid = httpUrl(url);
    if (!valid)
        return null;
    const path = new URL(valid).pathname.toLowerCase();
    mime = mime.split(';')[0].toLowerCase();
    if (/\.(m3u8|mpd)$/.test(path) || /mpegurl|dash\+xml/.test(mime))
        return 'stream';
    if (/\.(ts|m4s)$/.test(path) || mime === 'video/mp2t')
        return null;
    if (mime.startsWith('video/') || /\.(mp4|webm|mkv|mov|avi|flv|m4v)$/.test(path))
        return 'video';
    if (mime.startsWith('audio/') || /\.(mp3|m4a|aac|wav|flac|ogg|opus)$/.test(path))
        return 'audio';
    if (mime.startsWith('image/') || /\.(png|jpe?g|gif|webp|svg|avif|bmp|ico)$/.test(path))
        return 'image';
    if (/\.(exe|msi|msix|dmg|apk|deb|rpm|appimage)$/.test(path))
        return 'program';
    if (/\.(zip|rar|7z|tar|gz|tgz|bz2|xz|iso)$/.test(path))
        return 'archive';
    if (mime === 'application/pdf' || /\.(pdf|docx?|xlsx?|pptx?|txt|csv|epub)$/.test(path))
        return 'document';
    if (/attachment/i.test(disposition))
        return 'file';
    return null;
}
/**
 * 从成功的标签页响应生成资源值对象。Range 响应优先取完整总长度，不把一个分片的 Content-Length
 * 当成整文件；超过安全整数范围显示未知。
 */
export function fromHeaders(details) {
    if (details.tabId < 0 || details.statusCode < 200 || details.statusCode >= 300)
        return null;
    const headers = Object.fromEntries(
        (details.responseHeaders || []).map(h => [h.name.toLowerCase(), h.value || '']));
    const url = httpUrl(details.url);
    if (!url)
        return null;
    const mime = (headers['content-type'] || '').slice(0, 128);
    const disposition = headers['content-disposition'] || '';
    const kind = resourceKind(url, mime, disposition);
    if (!kind)
        return null;
    const range = /\/(\d+)$/.exec(headers['content-range'] || '');
    const size = Number(range ? range[1] : headers['content-length']);
    return {
        url,
        kind,
        name : suggestedName(url, disposition),
        mime,
        size : Number.isSafeInteger(size) && size >= 0 && (range || headers['content-length'])
                   ? size
                   : null
    };
}
/**
 * 验证用户主动粘贴的配对信息。只能指向固定 IPv4 loopback，不允许任意远程 endpoint 或把 Token
 * 发给其他主机；原样返回的 Token 不得写日志。
 */
export function parsePairing(text) {
    const value = typeof text === 'string' ? JSON.parse(text) : text;
    const u = new URL(value.endpoint);
    if (u.protocol !== 'http:' || u.hostname !== '127.0.0.1' || !u.port || u.username ||
        u.password || u.search || u.hash || (u.pathname && u.pathname !== '/') ||
        !/^[a-f0-9]{64}$/.test(value.token))
        throw new Error('请粘贴 QDM 桌面程序复制的完整配对信息');
    return {endpoint : u.origin, token : value.token};
}
/**
 * 按时间限制每标签和全局候选项，控制 storage.session
 * 大小。只删除本扩展会话候选，不改变浏览器下载记录或桌面文件。
 */
export function trimResources(store) {
    const all = [];
    for (const [tab, items] of Object.entries(store)) {
        store[tab] = items.slice(-MAX_PER_TAB);
        for (const item of store[tab])
            all.push({tab, item});
    }
    all.sort((a, b) => b.item.seen - a.item.seen);
    const keep = new Set(all.slice(0, MAX_TOTAL).map(x => x.item.id));
    for (const tab of Object.keys(store)) {
        store[tab] = store[tab].filter(x => keep.has(x.id));
        if (!store[tab].length)
            delete store[tab];
    }
    return store;
}

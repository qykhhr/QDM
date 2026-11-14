/** 仅在用户点击“扫描页面”时运行；读取可见 DOM 地址，不注入页面脚本或改写页面。 */
(() => {
    const items = [];
    for (const node of document.querySelectorAll('a[href],video,audio,source,img')) {
        const url = node.currentSrc || node.src || node.href;
        if (url && /^https?:/i.test(url))
            items.push({url, mime : node.type || (node.tagName === 'IMG' ? 'image/unknown' : '')});
        if (items.length >= 200)
            break;
    }
    return items;
})();

/**
 * 浏览器下载交接状态机。
 *
 * 顺序必须是：记录交接意图 → 暂停浏览器 → 桌面持久化确认 → 取消浏览器。
 * 不能先取消再发送，否则桌面断开时会丢掉用户原本可以继续的下载。
 * 日志只保存浏览器下载 ID、幂等编号和阶段，不重复保存 URL、Cookie 或文件路径。
 */
import {httpUrl, resourceKind} from './model.js';

export class DownloadTakeover {
    /**
     * @param {object} services 浏览器 API 与通信依赖，便于独立验证交接故障。
     * @param {object} services.downloads Chrome/Edge downloads API。
     * @param {object} services.storage 本机 storage.local，保存尚未结束的交接意图。
     * @param {Function} services.getConfig 返回接管开关；与资源嗅探开关独立。
     * @param {Function} services.createInQdm 必须在桌面任务已持久化后才 resolve。
     * @param {Function} services.isPost 判断近期是否观察到同 URL 的 POST 请求。
     */
    constructor(services) {
        Object.assign(this, services);
        this.running = new Set();
        this.writes = Promise.resolve();
    }

    /** 串行写日志，避免多个并发浏览器下载覆盖彼此的交接记录。 */
    async journal(id, entry) {
        const operation = this.writes.then(async () => {
            const {handoffs = {}} = await this.storage.get('handoffs');
            if (entry)
                handoffs[id] = entry;
            else
                delete handoffs[id];
            await this.storage.set({handoffs});
        });
        this.writes = operation.catch(() => {});
        return operation;
    }

    /** 从浏览器查询最新状态，不依赖 onCreated 中可能已经过时的状态快照。 */
    async current(id) {
        const items = await this.downloads.search({id : Number(id)});
        return items[0];
    }

    /**
     * 恢复尚未交给桌面的浏览器下载。
     * resume 失败时保留原下载及交接记录，不把失败伪装为“已接管”。
     */
    async restore(id) {
        const item = await this.current(id);
        if (item && item.state !== 'complete' && (item.paused || item.canResume)) {
            await this.downloads.resume(Number(id));
        }
        await this.journal(id, null);
    }

    /**
     * 处理新下载或恢复一次尚未完成的交接。
     * blob/data、隐私窗口、其他扩展创建、用户已暂停及已知 POST 下载保留给浏览器。
     * danger 标记不安全的下载也不接管，不尝试绕过浏览器安全判断。
     */
    async handle(item, existing = null) {
        if (!item || this.running.has(item.id))
            return;
        this.running.add(item.id);
        let entry = existing;
        try {
            const settings = await this.getConfig();
            if (existing?.phase === 'accepted') {
                // 桌面已经确认，worker 退出不能导致再次创建下载；只补完浏览器取消。
                if (item.state === 'in_progress')
                    await this.downloads.cancel(item.id);
                await this.journal(item.id, null);
                await this.onSuccess?.({task_id : existing.taskId});
                return;
            }
            if (settings.takeover === false) {
                if (entry)
                    await this.restore(item.id);
                return;
            }
            const url = httpUrl(item.finalUrl || item.url);
            if (!url || item.incognito || item.byExtensionId || item.state !== 'in_progress' ||
                (!entry && item.paused) ||
                (item.danger && !['safe', 'accepted'].includes(item.danger)) ||
                resourceKind(url, item.mime) === 'stream' || await this.isPost(url) ||
                (httpUrl(item.url) !== url && await this.isPost(httpUrl(item.url)))) {
                if (entry)
                    await this.restore(item.id);
                return;
            }
            if (!entry) {
                const {handoffs = {}} = await this.storage.get('handoffs');
                if (Object.keys(handoffs).length >= 16)
                    return;
                entry = {
                    requestId : crypto.randomUUID(),
                    phase : 'preparing',
                    createdAt : Date.now()
                };
                await this.journal(item.id, entry);
            }
            if (!item.paused)
                await this.downloads.pause(item.id);
            entry = {...entry, phase : 'paused'};
            await this.journal(item.id, entry);

            // 桌面端以同一个 requestId 去重；超时后恢复 worker 可安全重复发送。
            const latest = await this.current(item.id);
            if (!latest || latest.state !== 'in_progress') {
                // 用户在交接期间主动取消，或浏览器已完成，不再新增桌面下载。
                await this.journal(item.id, null);
                return;
            }
            if (latest?.danger && !['safe', 'accepted'].includes(latest.danger))
                throw new Error('浏览器正在检查下载，保留给浏览器处理');
            const filename = latest?.filename || item.filename;
            const name = filename ? filename.split(/[\\/]/).pop() : '';
            const result =
                await this.createInQdm({url, name, id : entry.requestId}, item.referrer || '');
            if (!result?.task_id)
                throw new Error('桌面程序未确认任务，已保留浏览器下载');
            entry = {...entry, phase : 'accepted', taskId : result.task_id};
            await this.journal(item.id, entry);
            await this.downloads.cancel(item.id);
            await this.journal(item.id, null);
            await this.onSuccess?.(result);
        } catch (error) {
            if (entry?.phase === 'accepted') {
                // 不恢复原浏览器传输：桌面已经拥有任务，恢复会形成两份并行下载。
                await this.onError?.('QDM 已接收任务，浏览器取消尚未完成，将自动重试：' +
                                     error.message);
            } else if (entry) {
                try {
                    await this.restore(item.id);
                    await this.onError?.('未能交给 QDM，已恢复浏览器下载：' + error.message);
                } catch (restoreError) {
                    await this.onError?.('浏览器下载已保留，请在下载页继续：' +
                                         restoreError.message);
                }
            }
        } finally {
            this.running.delete(item.id);
        }
    }

    /** worker/浏览器重启后恢复日志；不扫描和接管用户其他历史下载。 */
    async recover() {
        const {handoffs = {}} = await this.storage.get('handoffs');
        for (const [id, entry] of Object.entries(handoffs)) {
            const item = await this.current(id);
            if (!item || item.state === 'complete' ||
                (item.state === 'interrupted' && entry.phase === 'accepted')) {
                await this.journal(id, null);
            } else {
                await this.handle(item, entry);
            }
        }
    }
}

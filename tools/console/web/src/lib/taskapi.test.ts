/**
 * @file  taskapi.test.ts
 * @brief 用**假 fetch** 把烧写流程（分片 → 提交 → 回读）与"端点在不在"的分支钉死
 *
 * 桥还没实现 `/api/tasks*`，所以这里测的不是网络，而是协议约定：
 * 分片大小 120 B、offset 递增、提交带 len/crc、一半失败就不提交。
 * 端点一落地，这些断言就是验收标准。
 */
import { describe, expect, it } from 'vitest';

import { CHUNK_BYTES, bytesToHex, chunkImage, createTaskApi, hexToBytes, type TaskStatus } from './taskapi';

interface Call {
  url: string;
  method: string;
  body: unknown;
}

function fakeFetch(replies: { status?: number; json?: unknown; text?: string }[]) {
  const calls: Call[] = [];
  const fn = (url: string, init?: { method?: string; body?: string }) => {
    calls.push({ url, method: init?.method ?? 'GET', body: init?.body === undefined ? undefined : JSON.parse(init.body) });
    const r = replies.shift() ?? { status: 200, json: { ok: true } };
    return Promise.resolve({
      ok: (r.status ?? 200) < 400,
      status: r.status ?? 200,
      json: () => Promise.resolve(r.json ?? { ok: true }),
      text: () => Promise.resolve(r.text ?? ''),
    });
  };
  return { calls, fn };
}

describe('字节与分片', () => {
  it('hex 往返、容忍空格和 0x、奇数长度报错', () => {
    const b = new Uint8Array([0, 1, 0xab, 0xff]);
    expect(bytesToHex(b)).toBe('0001abff');
    expect([...hexToBytes('00 01 AB ff')]).toEqual([0, 1, 0xab, 0xff]);
    expect([...hexToBytes('0001')]).toEqual([0, 1]);
    expect([...hexToBytes('544B 0101')]).toEqual([0x54, 0x4b, 1, 1]);
    expect(() => hexToBytes('abc')).toThrow(/奇数/);
  });

  it(`切成 ${CHUNK_BYTES} B 一片，offset 递增，拼回来还是原样`, () => {
    const img = new Uint8Array(300);
    for (let i = 0; i < img.length; i += 1) img[i] = i & 0xff;
    const parts = chunkImage(img);
    expect(parts.map((p) => p.offset)).toEqual([0, 120, 240]);
    expect(parts.map((p) => p.hex.length / 2)).toEqual([120, 120, 60]);
    const joined = new Uint8Array(300);
    for (const p of parts) joined.set(hexToBytes(p.hex), p.offset);
    expect(bytesToHex(joined)).toBe(bytesToHex(img));
  });

  it('空镜像没有分片', () => {
    expect(chunkImage(new Uint8Array(0))).toEqual([]);
  });
});

describe('端点存在性', () => {
  it('404 当成"桥还没实现"，不是错误弹窗', async () => {
    const { fn } = fakeFetch([{ status: 404 }]);
    const r = await createTaskApi(fn).list();
    expect(r).toMatchObject({ ok: false, supported: false });
    if (!r.ok) expect(r.error).toContain('/api/tasks');
  });

  it('车没置 HOST_CAP_TASK 的报错也算"不支持"，别的错算真错', async () => {
    const a = fakeFetch([{ status: 200, json: { ok: false, error: '固件不支持任务功能' } }]);
    expect(await createTaskApi(a.fn).list()).toMatchObject({ ok: false, supported: false });

    const b = fakeFetch([{ status: 500, json: { ok: false, error: '运动中有任务在跑' } }]);
    expect(await createTaskApi(b.fn).list()).toMatchObject({ ok: false, supported: true });
  });

  it('桥不在（fetch 抛）也能说清楚', async () => {
    const api = createTaskApi(() => Promise.reject(new Error('ECONNREFUSED')));
    const r = await api.list();
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.error).toContain('连不上桥');
  });

  it('回的不是 JSON 也不崩', async () => {
    const bad = createTaskApi(() =>
      Promise.resolve({
        ok: true,
        status: 200,
        json: () => Promise.reject(new Error('Unexpected token <')),
        text: () => Promise.resolve('<html>'),
      }),
    );
    const r = await bad.list();
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.error).toContain('不是 JSON');
  });
});

describe('列表 / 读回 / 状态', () => {
  it('列表少了 items 就当空的', async () => {
    const a = fakeFetch([{ json: { ok: true, items: [{ id: 0, name: '循迹', len: 208, crc: 0x023a, flags: 0 }] } }]);
    const r = await createTaskApi(a.fn).list();
    expect(r.ok && r.data.items).toHaveLength(1);
    expect(r.ok && r.data.items[0].name).toBe('循迹');

    const b = fakeFetch([{ json: { ok: true } }]);
    const r2 = await createTaskApi(b.fn).list();
    expect(r2.ok && r2.data.items).toEqual([]);
    expect(r2.ok && r2.data.supported).toBe(true);
  });

  it('读回 hex → 字节；太短的报错', async () => {
    const a = fakeFetch([{ json: { ok: true, hex: '544b0101000000000000000000000000' } }, { json: { ok: true, hex: '544b' } }]);
    const api = createTaskApi(a.fn);
    const ok = await api.get(0);
    expect(ok.ok && bytesToHex(ok.data)).toBe('544b0101000000000000000000000000');
    expect(ok.ok && ok.data.length).toBe(16);
    const bad = await api.get(0);
    expect(bad.ok).toBe(false);
    if (!bad.ok) expect(bad.error).toContain('太短');
  });

  it('状态兼容两种回法（包一层 status 或直接平铺），没有就 null', async () => {
    const st: TaskStatus = { id: 0, state: 1, running: true, block: 3, elapsedMs: 420, flags: 0 };
    const a = fakeFetch([{ json: { ok: true, status: st } }, { json: { ok: true, ...st } }, { json: { ok: true, status: null } }]);
    const api = createTaskApi(a.fn);
    const wrapped = await api.status();
    expect(wrapped.ok && wrapped.data?.state).toBe(1);
    expect(wrapped.ok && wrapped.data?.elapsedMs).toBe(420);
    const flat = await api.status();
    expect(flat.ok && flat.data?.running).toBe(true);
    const none = await api.status();
    expect(none.ok && none.data).toBe(null);
  });
});

describe('烧写流程', () => {
  it('分片按 offset 递增发，最后提交带 len/crc/name', async () => {
    const img = new Uint8Array(250);
    const { calls, fn } = fakeFetch([{ json: { ok: true } }, { json: { ok: true } }, { json: { ok: true } }, { json: { ok: true } }]);
    const r = await createTaskApi(fn).burn(2, img, img.length, 0x1234, '循迹');
    expect(r).toMatchObject({ ok: true, data: { chunks: 3 } });
    expect(calls.map((c) => c.url)).toEqual(['/api/tasks/put', '/api/tasks/put', '/api/tasks/put', '/api/tasks/commit']);
    expect(calls.slice(0, 3).map((c) => (c.body as { offset: number }).offset)).toEqual([0, 120, 240]);
    expect(calls[0].method).toBe('POST');
    expect(calls[3].body).toEqual({ id: 2, len: 250, crc: 0x1234, name: '循迹' });
    const first = calls[0].body as { id: number; hex: string };
    expect(first.id).toBe(2);
    expect(first.hex.length / 2).toBe(120);
  });

  it('中途失败就不提交（车上不能留半个程序）', async () => {
    const { calls, fn } = fakeFetch([{ json: { ok: true } }, { status: 500, json: { ok: false, error: '运动中有任务在跑' } }]);
    const r = await createTaskApi(fn).burn(0, new Uint8Array(200), 200, 1);
    expect(r.ok).toBe(false);
    expect(calls.every((c) => c.url.endsWith('/put'))).toBe(true);
  });

  it('空镜像只提交（边角情况别死循环）', async () => {
    const { calls, fn } = fakeFetch([{ json: { ok: true } }]);
    const r = await createTaskApi(fn).burn(0, new Uint8Array(0), 0, 0);
    expect(r).toMatchObject({ ok: true, data: { chunks: 0 } });
    expect(calls).toHaveLength(1);
  });

  it('运行/停止/删除走各自的端点', async () => {
    const { calls, fn } = fakeFetch([{ json: { ok: true } }, { json: { ok: true } }, { json: { ok: true } }]);
    const api = createTaskApi(fn);
    await api.run(1);
    await api.stop();
    await api.del(1);
    expect(calls.map((c) => c.url)).toEqual(['/api/tasks/run', '/api/tasks/stop', '/api/tasks/delete']);
    expect(calls[0].body).toEqual({ id: 1 });
    expect(calls[2].body).toEqual({ id: 1 });
  });
});

/**
 * 「删不掉」是用户真实撞过的坑：开机自启的任务跑着时，固件对 DELETE 回 status 7（忙），
 * 页面只弹一句「被车拒绝：忙」，车就一直自己跑。delWithStop 必须把这段顺序做对。
 */
describe('删任务：跑着就先停车', () => {
  it('第一次被拒（忙）→ 停车 → 再删成功', async () => {
    const { calls, fn } = fakeFetch([
      { status: 200, json: { ok: false, error: '删除任务被车拒绝：忙（status=7）' } },
      { status: 200, json: { ok: true, status: 0, status_name: 'OK' } },
      { status: 200, json: { ok: true, status: 0, status_name: 'OK' } },
    ]);
    const r = await createTaskApi(fn).delWithStop(6);
    expect(r.stopped).toBe(true);
    expect(r.result.ok).toBe(true);
    expect(calls.map((c) => c.url)).toEqual(['/api/tasks/delete', '/api/tasks/stop', '/api/tasks/delete']);
    expect(calls[0].body).toEqual({ id: 6 });
    expect(calls[1].body).toEqual({});
    expect(calls[2].body).toEqual({ id: 6 });
  });

  it('一次就删掉时不去碰停车', async () => {
    const { calls, fn } = fakeFetch([{ json: { ok: true } }]);
    const r = await createTaskApi(fn).delWithStop(0);
    expect(r).toMatchObject({ stopped: false });
    expect(calls.map((c) => c.url)).toEqual(['/api/tasks/delete']);
  });

  it('停车也失败时不再硬删一次，把原错误带回去', async () => {
    const { calls, fn } = fakeFetch([
      { status: 200, json: { ok: false, error: '删除任务被车拒绝：忙（status=7）' } },
      { status: 200, json: { ok: false, error: '停止任务没人应答：超时' } },
    ]);
    const r = await createTaskApi(fn).delWithStop(6);
    expect(r.stopped).toBe(false);
    expect(r.result.ok).toBe(false);
    if (!r.result.ok) expect(r.result.error).toContain('忙');
    expect(calls.map((c) => c.url)).toEqual(['/api/tasks/delete', '/api/tasks/stop']);
  });
});

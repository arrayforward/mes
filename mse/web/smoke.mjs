/* 最小冒烟:mock 后端(契约样例数据)+ 静态托管,验证 index.html 引用与 app.js 的 API 路径均可达。
   用法: node smoke.mjs  (无第三方依赖,验证完自动退出) */
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('.', import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };

const mock = {
  '/meta/views': [
    { view_id: 'V-PLAN-A3', render_mode: '终态', queries: {}, selects: [], rules: [], emits: [], variants: { 计划员: { columns: [], emits: [] } }, status: 'ok' },
    { view_id: 'V-ANDON-D1', render_mode: '拦截', queries: {}, selects: [], rules: [], emits: [], variants: {}, status: 'ok' }
  ],
  '/meta/event-types': [{ type: 'ANDON_CALL', required_keys: ['level'], optional_keys: [], rules: [], correction: false, multi_target: false, settlement: 'sync', min_trust: 2, status: 'ok' }],
  '/meta/attributes': [{ key: 'level', semantic: '安灯级别', datatype: 'enum', unit: '', range: ['黄', '红'], writers: [], kind: '', status: 'ok' }],
  '/meta/anchors': [{ path: '整车厂/总装车间/总装线/工位01', name: '工位01' }],
  '/meta/ontologies': [{ id: 'CAR-001', keys: ['vin'] }],
  '/meta/events?limit=30': [],
  '/views/V-PLAN-A3': { view_id: 'V-PLAN-A3', render_mode: '终态', observer: '', as_of_seq: 0, columns: [], actions: [], rows: [] }
};

const server = http.createServer(async (req, res) => {
  const url = req.url.split('?')[0];
  const key = mock[req.url] !== undefined ? req.url : (mock[url] !== undefined ? url : null);
  if (key) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mock[key]));
    return;
  }
  if (req.method === 'POST' && (url === '/events' || url === '/drain')) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(url === '/drain' ? '{"drained":0}' : '{"status":"rejected","layer":2,"violations":["mock"]}');
    return;
  }
  try {
    const body = await readFile(join(root, url === '/' ? 'index.html' : url));
    res.writeHead(200, { 'Content-Type': MIME[extname(url)] || 'application/octet-stream' });
    res.end(body);
  } catch {
    res.writeHead(404); res.end('not found');
  }
});

await new Promise(r => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}`;
let fail = 0;
const check = async (name, ok) => { console.log((ok ? 'PASS' : 'FAIL') + ' ' + name); if (!ok) fail++; };

// 1) index.html 引用的本地资源
const htmlDoc = await (await fetch(base + '/index.html')).text();
const srcs = [...htmlDoc.matchAll(/<script src="([^"]+)"/g)].map(m => m[1]);
await check('index.html 引用 vendor+app.js(4 个)', srcs.length === 4);
for (const s of srcs) {
  const r = await fetch(base + '/' + s);
  const buf = await r.arrayBuffer();
  await check(`资源可达且非空 ${s} (${buf.byteLength}B)`, r.ok && buf.byteLength > 0);
}

// 2) app.js 内 API 路径字面量与 mock 契约一致
const appJs = await readFile(join(root, 'app.js'), 'utf8');
const paths = [...new Set([...appJs.matchAll(/api\('([^']+)'/g)].map(m => m[1]))];
console.log('app.js API 路径:', paths.join(', '));
for (const p of paths) {
  if (p.startsWith('/meta') || p.startsWith('/events') || p.startsWith('/drain')) {
    const r = await fetch(base + p, p === '/events' || p === '/drain' ? { method: 'POST' } : {});
    await check(`契约路径可达 ${p}`, r.ok);
  }
}
await check('包含 /views/ 拼接', appJs.includes("'/views/' + encodeURIComponent(selectedView)"));
await check('包含 X-MSE-Token 头', appJs.includes("'X-MSE-Token'"));

process.exitCode = fail ? 1 : 0;
server.close();

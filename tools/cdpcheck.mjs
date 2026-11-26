// Drives a real headless Edge over CDP and reads a page value back.
// Used to check the browser build end to end (workers + wasm + UI), which no
// static check can cover.
//
// usage: node cdpcheck.mjs <url> <expression> [waitMs]

const url = process.argv[2];
const expr = process.argv[3] || "document.title";
const waitMs = Number(process.argv[4] || 8000);

const base = 'http://127.0.0.1:9333';

async function targets() {
  const res = await fetch(base + '/json');
  return res.json();
}

async function main() {
  let page = null;
  for (let i = 0; i < 40 && !page; i++) {
    try {
      const list = await targets();
      page = list.find((t) => t.type === 'page');
    } catch {
      /* the debugger endpoint is not up yet */
    }
    if (!page) await new Promise((r) => setTimeout(r, 250));
  }
  if (!page) throw new Error('no page target');

  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let id = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  };
  const send = (method, params) => new Promise((res) => {
    const myId = ++id;
    pending.set(myId, res);
    ws.send(JSON.stringify({ id: myId, method, params }));
  });

  await send('Page.enable', {});
  await send('Runtime.enable', {});
  await send('Page.navigate', { url });
  await new Promise((r) => setTimeout(r, waitMs));

  const out = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  const value = out.result && out.result.result ? out.result.result.value : JSON.stringify(out);
  console.log(typeof value === 'string' ? value : JSON.stringify(value, null, 2));
  ws.close();
}

main().catch((e) => { console.error('FAILED', e.message); process.exit(1); });

// Worker for the browser build: loads the WebAssembly search core and answers
// one (direction, spawn cell) task at a time.
//
// Each worker owns its own instance and therefore its own transposition table,
// which keeps the four root directions independent of each other.

let core = null;

const imports = {
  env: {
    abort() {
      throw new Error('wasm abort');
    },
  },
};

self.onmessage = async (e) => {
  const msg = e.data;

  if (msg.type === 'init') {
    try {
      // The main thread compiles the module once and posts it here, so seven
      // other workers do not repeat the same compilation.
      if (msg.module) {
        core = (await WebAssembly.instantiate(msg.module, imports)).exports;
      } else {
        const res = await fetch(msg.wasm);
        const bytes = await res.arrayBuffer();
        core = (await WebAssembly.instantiate(bytes, imports)).exports;
      }
      core.init();
      core.clearTT();
      self.postMessage({ type: 'ready' });
    } catch (err) {
      self.postMessage({ type: 'failed', error: String(err) });
    }
    return;
  }

  if (msg.type === 'task') {
    if (!core) return;
    const { id, rows, dir, shard, nShard, move, depth } = msg;
    const t0 = performance.now();
    const v = core.scoreCellMove(rows[0], rows[1], rows[2], rows[3], dir, shard, nShard, move, depth);
    // Reported back so the pool can show how much of the wall clock was real
    // work rather than dispatch.
    self.postMessage({ type: 'result', id, v, t: performance.now() - t0 });
  }
};

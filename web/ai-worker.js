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
      const res = await fetch(msg.wasm);
      const bytes = await res.arrayBuffer();
      const { instance } = await WebAssembly.instantiate(bytes, imports);
      core = instance.exports;
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
    const { id, rows, dir, shard, nShard, depth } = msg;
    const v = core.scoreShard(rows[0], rows[1], rows[2], rows[3], dir, shard, nShard, depth);
    self.postMessage({ type: 'result', id, v });
  }
};

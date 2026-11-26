// Browser search engine: a pool of workers sharing the root chance node.
//
// The root position is cut the same way the desktop build cuts it: one task per
// (direction, spawn cell). Tasks are pulled from a queue by whichever worker is
// free, and the per-direction sums are formed in a fixed order so the chosen
// move does not depend on which worker happened to run first.
//
// Kept deliberately dependency free: it is loaded by index.html, by the WebView2
// host and by the Node benchmark.

(function (global) {
  const MAX_WORKERS = 8;

  let workers = [];
  let core = null;          // main-thread instance, used for board arithmetic only
  let readyPromise = null;
  let ready = false;
  let taskSeq = 0;
  const pending = new Map();

  // Depth is chosen per move from the measured cost of the previous one, which
  // keeps a phone and a workstation at a similar pace without a settings screen.
  let depth = 4;
  const DEPTH_MIN = 3;
  const DEPTH_MAX = 6;
  const TARGET_MS = 160;

  function workerCount() {
    const hw = navigator.hardwareConcurrency || 4;
    return Math.max(1, Math.min(hw, MAX_WORKERS));
  }

  function spawnWorker() {
    const w = new Worker('ai-worker.js');
    w.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'ready') return;
      if (m.type === 'failed') {
        console.error('worker failed:', m.error);
        if (global.LiuEngine) global.LiuEngine.lastError = m.error;
        return;
      }
      if (m.type === 'result') {
        const resolve = pending.get(m.id);
        if (resolve) {
          pending.delete(m.id);
          resolve(m.v);
        }
      }
    };
    return w;
  }

  async function initCore() {
    if (core) return core;
    const res = await fetch('wasm/solver.wasm');
    const bytes = await res.arrayBuffer();
    const { instance } = await WebAssembly.instantiate(bytes, {
      env: { abort() { throw new Error('wasm abort'); } },
    });
    core = instance.exports;
    core.init();
    core.clearTT();
    return core;
  }

  function init() {
    if (readyPromise) return readyPromise;
    readyPromise = (async () => {
      await initCore();
      return new Promise((resolve) => {
      const n = workerCount();
      let pendingReady = n;
      for (let i = 0; i < n; i++) {
        const w = spawnWorker();
        workers.push(w);
        const onReady = (e) => {
          if (e.data && e.data.type === 'ready') {
            w.removeEventListener('message', onReady);
            if (--pendingReady === 0) { ready = true; resolve(workers.length); }
            else if (global.LiuEngine) global.LiuEngine.booted = n - pendingReady;
          }
        };
        w.addEventListener('message', onReady);
        w.postMessage({ type: 'init', wasm: 'wasm/solver.wasm' });
      }
      });
    })();
    return readyPromise;
  }

  // Empty cells left after each move; 0 means the direction is illegal.
  function freeCounts(rows) {
    const out = [0, 0, 0, 0];
    for (let d = 0; d < 4; d++) out[d] = core.freeAfterMove(rows[0], rows[1], rows[2], rows[3], d);
    return out;
  }

  // Applies a move: returns {rows, gain} or null when nothing moves.
  function applyMove(rows, dir) {
    const g = core.doMove(rows[0], rows[1], rows[2], rows[3], dir);
    if (g < 0) return null;
    return { rows: [core.outR0(), core.outR1(), core.outR2(), core.outR3()], gain: g };
  }

  // Runs one task on a free worker. The pool is small, so a plain round-robin
  // over a shared index is enough to keep every core busy.
  let rr = 0;
  function runTask(task) {
    return new Promise((resolve) => {
      const id = ++taskSeq;
      pending.set(id, resolve);
      const w = workers[rr++ % workers.length];
      w.postMessage({ type: 'task', id, rows: task.rows, dir: task.dir, shard: task.shard, nShard: task.nShard, depth: task.depth });
    });
  }

  // Number of spawn cells for a direction, computed on the main thread from the
  // same tables the workers use. Filled in by the caller-provided helper.
  function buildTasks(rows, freeCounts, d) {
    const tasks = [];
    for (let s = 0; s < freeCounts[d]; s++) tasks.push({ rows, dir: d, shard: s, nShard: freeCounts[d], depth: d });
    return tasks;
  }

  // Last-resort path: the same tasks, run on the main thread. Used when the
  // worker pool cannot start (restricted contexts) or is still booting, so the
  // page always plays.
  function evaluateSync(rows, freeCounts, depthNow) {
    const scores = [-Infinity, -Infinity, -Infinity, -Infinity];
    for (let d = 0; d < 4; d++) {
      const n = freeCounts[d] | 0;
      if (n <= 0) continue;
      let sum = 0;
      for (let s = 0; s < n; s++) {
        sum += core.scoreShard(rows[0], rows[1], rows[2], rows[3], d, s, n, depthNow);
      }
      scores[d] = sum;
    }
    return scores;
  }

  // scores: [up, right, down, left]; freeCounts: same indexing, 0 = illegal.
  async function evaluate(rows, freeCounts) {
    const t0 = performance.now();
    if (!ready) {
      // Boot the pool, but never block the first move on it.
      init();
      const depthNow = ready ? depth : Math.max(DEPTH_MIN, depth - 1);
      const scores = evaluateSync(rows, freeCounts, depthNow);
      return { scores, ms: performance.now() - t0, depth: depthNow, fallback: true };
    }

    const jobs = [];
    for (let d = 0; d < 4; d++) {
      const n = freeCounts[d] | 0;
      if (n <= 0) continue;
      for (let s = 0; s < n; s++) {
        jobs.push({ rows, dir: d, shard: s, nShard: n, depth });
      }
    }
    if (jobs.length === 0) return { scores: [-Infinity, -Infinity, -Infinity, -Infinity], ms: 0, depth };

    const values = await Promise.all(jobs.map(runTask));

    const scores = [-Infinity, -Infinity, -Infinity, -Infinity];
    let k = 0;
    for (let d = 0; d < 4; d++) {
      const n = freeCounts[d] | 0;
      if (n <= 0) continue;
      let sum = 0;
      let ok = true;
      for (let s = 0; s < n; s++, k++) {
        const v = values[k];
        if (!(v > -1e29)) { ok = false; break; }
        sum += v;
      }
      if (ok) scores[d] = sum;
    }

    const ms = performance.now() - t0;

    // One-ply-at-a-time pace control: the next move searches deeper when this one
    // was cheap, and shallower when the device is struggling.
    if (ms > TARGET_MS * 1.6 && depth > DEPTH_MIN) depth -= 1;
    else if (ms < TARGET_MS * 0.55 && depth < DEPTH_MAX) depth += 1;

    return { scores, ms, depth };
  }

  global.LiuEngine = {
    get ready() { return ready; },
    booted: 0,
    lastError: '',
    init,
    evaluate,
    freeCounts,
    applyMove,
    get depth() { return depth; },
    set depth(v) { depth = Math.max(DEPTH_MIN, Math.min(DEPTH_MAX, v | 0)); },
    workerCount,
  };
})(typeof self !== 'undefined' ? self : this);

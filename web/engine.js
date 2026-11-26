// Browser search engine: a pool of workers sharing the root chance node.
//
// The root position is cut the same way the desktop build cuts it: one task per
// (direction, spawn cell). Tasks go into one queue, and a worker takes the next
// task as soon as it finishes its own - fixed round-robin assignment leaves the
// tail of a move waiting on one worker while the others sit idle, which is
// exactly the cost that makes a browser build feel slow.
//
// Kept deliberately dependency free: it is loaded by index.html, by the WebView2
// host and by the Node benchmark.

(function (global) {
  const MAX_WORKERS = 16;
  const VERSION = '6';          // bump together with index.html to defeat caches
  const query = typeof location !== 'undefined' ? new URLSearchParams(location.search) : new URLSearchParams();

  let workers = [];
  let core = null;              // main-thread instance, used for board arithmetic
  let wasmModule = null;        // compiled once, then handed to every worker
  let readyPromise = null;
  let ready = false;
  let taskSeq = 0;
  const pending = new Map();     // task id -> resolve
  let taskMs = 0;                // sum of worker-reported task durations
  let taskCount = 0;
  const queue = [];              // tasks waiting for a free worker
  const idle = [];               // worker indices currently free
  const busy = [];

  // Depth per move. Measured on sixteen workers: depth 4 costs 20-30 ms, depth 5
  // costs 190-400 ms - one more ply is thirteen times the work here, because the
  // table has nothing cached for it yet. So the open board plays at four plies,
  // and only the crowded endgame, where the branch factor collapses, pays for
  // five. That keeps the pace at tens of moves per second without weakening the
  // part of the game that decides the score.
  let depth = 4;
  const DEPTH_MIN = 3;
  const DEPTH_MAX = 6;

  const forcedDepth = Number(query.get('depth') || 0);
  if (forcedDepth > 0) depth = Math.max(DEPTH_MIN, Math.min(DEPTH_MAX, forcedDepth | 0));

  function workerCount() {
    const forced = Number(query.get('workers') || 0);
    if (forced > 0) return Math.max(1, Math.min(forced, MAX_WORKERS));
    const hw = navigator.hardwareConcurrency || 4;
    return Math.max(1, Math.min(hw, MAX_WORKERS));
  }

  // Hands the next queued task to every worker that is free. Called when tasks
  // are queued and again whenever a worker reports back.
  function pump() {
    while (idle.length > 0 && queue.length > 0) {
      const idx = idle.pop();
      const job = queue.shift();
      busy[idx] = 1;
      workers[idx].postMessage(job.msg);
    }
  }

  function spawnWorker(idx) {
    const w = new Worker('ai-worker.js?v=' + VERSION);
    w.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'ready') return;
      if (m.type === 'failed') {
        console.error('worker failed:', m.error);
        if (global.LiuEngine) global.LiuEngine.lastError = m.error;
        return;
      }
      if (m.type === 'result') {
        taskMs += m.t || 0;
        taskCount += 1;
        const resolve = pending.get(m.id);
        if (resolve) {
          pending.delete(m.id);
          resolve(m.v);
        }
        busy[idx] = 0;
        idle.push(idx);
        pump();
      }
    };
    return w;
  }

  async function initCore() {
    if (core) return core;
    const res = await fetch('wasm/solver.wasm?v=' + VERSION);
    const bytes = await res.arrayBuffer();
    // Compile once here and post the module to the workers: compiling the same
    // 13 KB module in eight workers costs more than the first few moves.
    wasmModule = await WebAssembly.compile(bytes);
    const instance = await WebAssembly.instantiate(wasmModule, {
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
          const w = spawnWorker(i);
          workers.push(w);
          const onReady = (e) => {
            if (e.data && e.data.type === 'ready') {
              w.removeEventListener('message', onReady);
              busy[i] = 0;
              idle.push(i);
              if (--pendingReady === 0) {
                ready = true;
                resolve(workers.length);
              } else if (global.LiuEngine) {
                global.LiuEngine.booted = n - pendingReady;
              }
            }
          };
          w.addEventListener('message', onReady);
          w.postMessage({ type: 'init', module: wasmModule, wasm: 'wasm/solver.wasm' });
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
      const depthNow = Math.max(DEPTH_MIN, depth - 1);
      const scores = evaluateSync(rows, freeCounts, depthNow);
      return { scores, ms: performance.now() - t0, depth: depthNow, fallback: true };
    }

    // Longest job first: a direction with more spawn cells has more work, so it
    // enters the queue before the cheaper ones. With a shared queue this keeps
    // the tail short without needing a cost estimate per task.
    const order = [0, 1, 2, 3].filter((d) => (freeCounts[d] | 0) > 0);
    order.sort((a, b) => freeCounts[b] - freeCounts[a]);

    // Eight tasks per spawn cell: two tile values times four moves. A single
    // cell is up to a quarter of a second at depth 5, so this is the level of
    // granularity that lets sixteen workers actually finish together.
    const jobs = [];
    for (const d of order) {
      const n = freeCounts[d] | 0;
      for (let s = 0; s < n; s++) {
        for (let mv = 0; mv < 8; mv++) jobs.push({ d, s, n, mv });
      }
    }
    if (jobs.length === 0) return { scores: [-Infinity, -Infinity, -Infinity, -Infinity], ms: 0, depth };

    const promises = jobs.map((job) => new Promise((resolve) => {
      const id = ++taskSeq;
      pending.set(id, resolve);
      queue.push({
        id,
        msg: {
          type: 'task', id, rows,
          dir: job.d, shard: job.s, nShard: job.n, move: job.mv, depth,
        },
      });
    }));
    pump();

    const values = await Promise.all(promises);

    const scores = [-Infinity, -Infinity, -Infinity, -Infinity];
    let k = 0;
    for (const d of order) {
      const n = freeCounts[d] | 0;
      let sum = 0;
      for (let s = 0; s < n; s++) {
        let v2 = 0;
        let v4 = 0;
        for (let mv = 0; mv < 4; mv++) { const v = values[k++]; if (v > v2) v2 = v; }
        for (let mv = 4; mv < 8; mv++) { const v = values[k++]; if (v > v4) v4 = v; }
        // The search core floors an all-blocked max node at zero, so the same
        // floor is applied here rather than propagating the sentinel.
        sum += (0.9 * Math.max(0, v2) + 0.1 * Math.max(0, v4)) / n;
      }
      scores[d] = sum;
    }

    const ms = performance.now() - t0;

    // Depth for the next move: five plies only when the board is crowded enough
    // that five is affordable. Derived from the position, not from the previous
    // move's clock - a table hit can make a deep search look cheap, and a
    // time-driven controller then oscillates between two depths every move.
    if (!forcedDepth) {
      let minFree = 16;
      for (let d = 0; d < 4; d++) {
        const n = freeCounts[d] | 0;
        if (n > 0 && n < minFree) minFree = n;
      }
      depth = minFree <= 5 ? 5 : 4;
    }

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
    stats() {
      return { workers: workers.length, idle: idle.length, queued: queue.length, depth, taskMs, taskCount };
    },
  };
})(typeof self !== 'undefined' ? self : this);

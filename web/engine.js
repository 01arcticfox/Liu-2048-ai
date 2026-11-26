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
  const VERSION = '9';          // bump together with index.html to defeat caches
  const query = typeof location !== 'undefined' ? new URLSearchParams(location.search) : new URLSearchParams();

  let workers = [];
  let core = null;              // main-thread instance, used for board arithmetic
  let wasmModule = null;        // compiled once, then handed to every worker
  let readyPromise = null;
  let ready = false;
  let taskSeq = 0;
  const pending = new Map();     // task id -> callback(value)
  let taskMs = 0;                // sum of worker-reported task durations
  let taskCount = 0;
  const queue = [];              // tasks waiting for a free worker
  let qhead = 0;                 // index of the next task to hand out
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

  // Depth policy: four plies while the board is open, five once it is crowded.
  // The page sets this once (see applySpeed) and the speed button no longer
  // touches it - capping depth at 8x made the fastest setting the weakest.
  let depthCap = 5;
  let endgameFree = 5;

  const qCapRaw = query.get('cap');
  const qFreeRaw = query.get('endfree');
  if (qCapRaw !== null) depthCap = Math.max(DEPTH_MIN, Math.min(Number(qCapRaw) | 0, DEPTH_MAX));
  if (qFreeRaw !== null) endgameFree = Math.max(0, Math.min(Number(qFreeRaw) | 0, 15));

  const forcedDepth = Number(query.get('depth') || 0);
  if (forcedDepth > 0) depth = Math.max(DEPTH_MIN, Math.min(DEPTH_MAX, forcedDepth | 0));

  function workerCount() {
    const forced = Number(query.get('workers') || 0);
    if (forced > 0) return Math.max(1, Math.min(forced, MAX_WORKERS));
    const hw = navigator.hardwareConcurrency || 4;
    return Math.max(1, Math.min(hw, MAX_WORKERS));
  }

  // Hands the next queued task to every worker that is free. Called when tasks
  // are queued and again whenever a worker reports back. The head index is
  // advanced instead of shifting the array: a move queues a hundred-odd tasks
  // and every shift would move the whole tail.
  function pump() {
    while (idle.length > 0 && qhead < queue.length) {
      const idx = idle.pop();
      const job = queue[qhead];
      queue[qhead++] = null;
      busy[idx] = 1;
      workers[idx].postMessage(job.msg);
    }
    if (qhead === queue.length) {
      queue.length = 0;
      qhead = 0;
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
        const cb = pending.get(m.id);
        if (cb) {
          pending.delete(m.id);
          cb(m.v);
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
    core.setTTBits(ttBits());
    core.clearTT();
    return core;
  }

  // Table size by device. The native build settled on 2^18 entries (4 MB per
  // thread) because that fits in L2 and measured 1.6x faster than a 64 MB table;
  // the browser pool follows the same reasoning. 2^19 removes about 13% of the
  // work but loses the same amount again to cache misses, and costs 6 MB per
  // worker, which a phone cannot spare.
  function ttBits() {
    const forced = Number(query.get('tt') || 0);
    if (forced >= 12 && forced <= 21) return forced | 0;
    const gb = navigator.deviceMemory || 4;
    return gb >= 4 ? 18 : 17;
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
          w.postMessage({ type: 'init', module: wasmModule, wasm: 'wasm/solver.wasm', ttBits: ttBits() });
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
    const total = jobs.length;
    if (total === 0) return { scores: [-Infinity, -Infinity, -Infinity, -Infinity], ms: 0, depth };

    // One promise and one counter for the whole move instead of a promise per
    // task: a full move is a hundred-odd tasks, and at eight moves a second the
    // per-task bookkeeping was visible on the main thread.
    const values = new Array(total);
    let arrived = 0;
    let settleAll = null;
    const allDone = new Promise((resolve) => { settleAll = resolve; });

    for (let i = 0; i < total; i++) {
      const job = jobs[i];
      const id = ++taskSeq;
      pending.set(id, (v) => {
        values[i] = v;
        if (++arrived === total) settleAll();
      });
      queue.push({
        id,
        msg: {
          type: 'task', id, rows,
          dir: job.d, shard: job.s, nShard: job.n, move: job.mv, depth,
        },
      });
    }
    pump();

    await allDone;

    const scores = [-Infinity, -Infinity, -Infinity, -Infinity];
    let k = 0;
    for (const d of order) {
      const n = freeCounts[d] | 0;
      let sum = 0;
      for (let s = 0; s < n; s++) {
        let v2 = 0;
        let v4 = 0;
        for (let mv = 0; mv < 4; mv++) { const v = values[k++] ?? 0; if (v > v2) v2 = v; }
        for (let mv = 4; mv < 8; mv++) { const v = values[k++] ?? 0; if (v > v4) v4 = v; }
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
      // Five plies only where the branch factor has collapsed enough to pay for
      // them. The threshold and the ceiling both come from the tuning below.
      depth = minFree <= endgameFree ? Math.min(depthCap, 5) : Math.min(depthCap, 4);
      if (depth < DEPTH_MIN) depth = DEPTH_MIN;
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
    // cap: highest depth the pool may use; free: how many empty cells (after the
    // quietest move) earn the extra ply.
    setTuning(cap, free) {
      depthCap = Math.max(DEPTH_MIN, Math.min(DEPTH_MAX, cap | 0));
      endgameFree = Math.max(0, Math.min(15, free | 0));
    },
    get tuning() { return { depthCap, endgameFree }; },
    workerCount,
    stats() {
      return { workers: workers.length, idle: idle.length, queued: queue.length - qhead, depth, taskMs, taskCount };
    },
  };
})(typeof self !== 'undefined' ? self : this);

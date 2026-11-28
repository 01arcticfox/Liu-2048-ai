
(() => {
  'use strict';

  const SIZE = 4;
  const VEC = { 0: [-1, 0], 1: [0, 1], 2: [1, 0], 3: [0, -1] }; // 0 up, 1 right, 2 down, 3 left
  const DIR_GLYPH = { 0: '↑', 1: '→', 2: '↓', 3: '←' };
  const DIR_ROT = { 0: -90, 1: 0, 2: 90, 3: 180 };
  const STORE = { best: 'b2048.best', lang: 'b2048.lang', theme: 'b2048.theme', speed: 'b2048.speed', save: 'b2048.save' };

  const $ = (sel) => document.querySelector(sel);

  const boardEl = $('#board');
  const gridEl = $('#grid');
  const tilesEl = $('#tiles');
  const scoreEl = $('#score');
  const bestEl = $('#best');
  const aiDot = $('#aiDot');
  const aiState = $('#aiState');
  const aiEval = $('#aiEval');
  const maxTileEl = $('#maxTile');
  const autoBtn = $('#autoBtn');
  const overlay = $('#overlay');
  const hintEl = $('#hint');
  const toastEl = $('#toast');
  const langBtn = $('#langBtn');
  const themeBtn = $('#themeBtn');
  const speedBtn = $('#speedBtn');
  const scoreBox = $('#scoreBox');

  /* ------------------------------------------------ i18n */
  const DICT = {
    zh: {
      sub: 'Bitboard 期望搜索 · 目标 2<sup>15</sup> = 32768',
      score: '得分', best: '最高',
      newGame: '新游戏', restart: '重新开始', auto: 'AI 自动', hint: 'AI 提示',
      tips: 'WASD / 方向键 · 鼠标拖拽 · 触屏滑动',
      idle: 'AI 待机', thinking: 'AI 思考中…', analyzing: 'AI 分析中…', running: 'AI 自动运行',
      suggest: '建议', evalNone: '评估 —', evalWord: '评估', maxPrefix: '最大',
      noMove: '无可行方向', ended: '已结束', browserOnly: '浏览器预览模式',
      needDesktop: 'AI 引擎需要桌面版（WebView2）运行',
      noUndo: '没有可撤销的步骤', undone: '已撤销', stuck: 'AI 无法继续',
      over: 'GAME OVER', again: '确定',
      kept: '终局已保留 · 按方向键开新局',
      overText: (m, s) => `最大方块 ${m} · 得分 ${s}`,
      win: '32768 达成', winBtn: '确定',
      winText: '已经拼出 2¹⁵，继续冲击 2¹⁶？',
      toLight: '切换浅色主题', toDark: '切换深色主题', langTitle: 'Switch to English',
      speedTitle: 'AI 速度',
    },
    en: {
      sub: 'Bitboard expectimax · target 2<sup>15</sup> = 32768',
      score: 'Score', best: 'Best',
      newGame: 'New Game', restart: 'Restart', auto: 'Auto AI', hint: 'Hint',
      tips: 'WASD / Arrows · Drag · Swipe',
      idle: 'AI idle', thinking: 'Thinking…', analyzing: 'Analyzing…', running: 'Auto running',
      suggest: 'Suggest', evalNone: 'Eval —', evalWord: 'Eval', maxPrefix: 'Max',
      noMove: 'No moves', ended: 'Finished', browserOnly: 'Browser preview',
      needDesktop: 'AI engine runs in the desktop app (WebView2)',
      noUndo: 'Nothing to undo', undone: 'Undone', stuck: 'AI stalled',
      over: 'GAME OVER', again: 'OK',
      kept: 'Final board kept · an arrow key starts a new game',
      overText: (m, s) => `Max tile ${m} · Score ${s}`,
      win: '32768 Reached', winBtn: 'OK',
      winText: 'The AI built 2¹⁵ — go for 2¹⁶?',
      toLight: 'Light theme', toDark: 'Dark theme', langTitle: '切换到中文',
      speedTitle: 'AI speed',
    },
  };

  let lang = localStorage.getItem(STORE.lang) === 'en' ? 'en' : 'zh';
  let theme = localStorage.getItem(STORE.theme) === 'light' ? 'light' : 'dark';

  // ?lang=en&theme=light can be forced, which makes sharing and previewing easy
  const qs = new URLSearchParams(location.search);
  if (qs.get('lang') === 'en' || qs.get('lang') === 'zh') lang = qs.get('lang');
  if (qs.get('theme') === 'light' || qs.get('theme') === 'dark') theme = qs.get('theme');

  const t = (key, ...args) => {
    const v = (DICT[lang] && DICT[lang][key]) || DICT.zh[key] || key;
    return typeof v === 'function' ? v(...args) : v;
  };

  /* ------------------------------------------------ state */
  let cells = Array.from({ length: SIZE }, () => Array(SIZE).fill(null));
  const tiles = new Map();
  let nextId = 1;
  let score = 0;
  let best = Number(localStorage.getItem(STORE.best) || 0);
  let history = [];
  let animating = false;
  let pendingStep = null;
  let queuedDir = -1;        // one slot of lookahead for human input
  let token = 0;
  let gameOver = false;
  let pendingRestart = false;   // 终局遮罩已关闭、盘面仍保留，等下一次输入再开新局
  let reachedGoal = false;

  let autoOn = false;
  let aiMode = null;
  let reqId = 0;
  let pendingSig = null;
  let stuckCounter = 0;

  const ui = { aiKey: 'idle', dotCls: 'off', dir: null, ms: 0, evalV: null, maxTile: 2, overlay: null };

  /* ------------------------------------------------ speed */
  const SPEEDS = [1, 2, 4, 8];
  let speedIdx = 0;
  let moveMsNow = 130;        // autopilot pace, chosen with the speed button
  let currentMs = -1;         // what the board is currently set to

  // A human move plays at a fixed, readable pace. The speed button exists for
  // the autopilot: when a person is driving, a slide that finishes inside one
  // frame is unreadable, and one that takes a third of a second feels stuck.
  // 110 ms matches what the established 2048 pages use for a slide.
  const HUMAN_MS = 110;

  function setSlideMs(ms) {
    if (ms === currentMs) return;
    currentMs = ms;
    // Scoped to the board instead of :root. A custom property on the document
    // element invalidates style for the entire tree, and only tiles read it.
    boardEl.style.setProperty('--move-ms', ms + 'ms');
  }

  function applySpeed(save) {
    // 112 ms base: at the default 4x a slide runs 28 ms, which keeps four to
    // eight moves a second legible while the board stays ahead of the hand.
    moveMsNow = Math.max(12, Math.round(112 / SPEEDS[speedIdx]));

    // The button changes the pace of the slides only. It used to cap the search
    // as well - 8x played at four plies with no fifth ply in the endgame - which
    // made the strongest-looking setting the weakest player. Depth is a property
    // of the position, not of how fast the tiles are drawn.
    // Ceiling five plies, but the fifth only when every direction leaves three
    // cells or fewer. The old rule read the narrowest direction, so one crowded
    // lane bought five plies while the rest of the board was wide open - and
    // five plies on a wide board is a half-second freeze per move, measured as
    // p95 502 ms against 58 ms with the pessimistic rule.
    const engine = window.LiuEngine;
    if (engine && engine.setTuning) engine.setTuning(5, 3);

    speedBtn.textContent = SPEEDS[speedIdx] + 'x';
    speedBtn.title = t('speedTitle') + ' · ' + SPEEDS[speedIdx] + 'x';
    if (save) localStorage.setItem(STORE.speed, String(speedIdx));
  }

  const nativeHost = () => !!(window.chrome && window.chrome.webview && window.chrome.webview.postMessage);
  const localEngine = () => !!window.LiuEngine;
  const bridge = () => nativeHost() || localEngine();
  const post = (msg) => {
    if (nativeHost()) { window.chrome.webview.postMessage(msg); return; }
    if (localEngine() && msg.startsWith('AI ')) localAsk(msg);
  };

  // Browser build: the same request goes to the worker pool instead of the
  // native host. Values travel as tile numbers (2, 4, ...) and become 4-bit
  // ranks here, which is what the search core understands.
  async function localAsk(msg) {
    const parts = msg.split(' ');
    const id = Number(parts[1]);
    const vals = parts.slice(2).map(Number);
    const rows = [0, 0, 0, 0];
    for (let i = 0; i < 16; i++) {
      const v = vals[i];
      if (!v) continue;
      rows[i >> 2] |= Math.round(Math.log2(v)) << ((i & 3) * 4);
    }
    try {
      window.LiuEngine.init();   // never block a move on the pool booting
      const counts = window.LiuEngine.freeCounts(rows);
      const r = await window.LiuEngine.evaluate(rows, counts);
      let best = -1;
      let bestV = -Infinity;
      for (let d = 0; d < 4; d++) {
        if (r.scores[d] > bestV) { bestV = r.scores[d]; best = d; }
      }
      if (!(bestV > -1e29)) best = -1;
      window.__aiRes(id, best, r.scores[0], r.scores[1], r.scores[2], r.scores[3], Math.round(r.ms));
    } catch (err) {
      console.error(err);
      window.__aiRes(id, -1, 0, 0, 0, 0, 0);
    }
  }

  function attemptMove(preferred, scores) {
    if (move(preferred)) return true;
    // Fall back through the scored directions when the preferred one is illegal
    const order = [0, 1, 2, 3].sort((a, b) => (scores[b] ?? -1e30) - (scores[a] ?? -1e30));
    for (const d of order) {
      if (d === preferred) continue;
      if (move(d)) return true;
    }
    return false;
  }

  window.addEventListener('error', (e) => {
    post('ERR ' + (e.message || 'error') + ' @' + (e.lineno || 0) + ':' + (e.colno || 0));
  });
  window.addEventListener('unhandledrejection', (e) => {
    post('ERR promise ' + (e.reason && e.reason.message ? e.reason.message : e.reason));
  });

  /* ------------------------------------------------ theme / language */
  function applyTheme() {
    document.documentElement.dataset.theme = theme;
    themeBtn.textContent = theme === 'dark' ? '☾' : '☀';
    themeBtn.title = theme === 'dark' ? t('toLight') : t('toDark');
    post('THEME ' + theme);
  }

  function renderStatus() {
    let label = t(ui.aiKey);
    if (ui.aiKey === 'suggest' && ui.dir != null) {
      label = t('suggest') + ' ' + DIR_GLYPH[ui.dir];
      if (ui.ms) label += ' · ' + ui.ms + 'ms';
    }
    aiState.textContent = label;
    aiDot.className = 'dot' + (ui.dotCls && ui.dotCls !== 'off' ? ' ' + ui.dotCls : '');
    aiState.parentElement.classList.toggle('busy', ui.dotCls === 'busy');

    aiEval.textContent = ui.evalV == null
      ? t('evalNone')
      : t('evalWord') + ' ' + fmtScore(ui.evalV) + (ui.ms ? ' · ' + ui.ms + 'ms' : '');
    maxTileEl.textContent = t('maxPrefix') + ' ' + ui.maxTile;
  }

  function renderOverlay() {
    if (!ui.overlay || overlay.hidden) return;
    const o = ui.overlay;
    $('#ovTitle').textContent = t(o.kind === 'win' ? 'win' : 'over');
    $('#ovText').textContent = o.kind === 'win' ? t('winText') : t('overText', o.max, fmt(o.score));
    $('#ovBtn').textContent = t(o.kind === 'win' ? 'winBtn' : 'again');
    overlay.classList.toggle('keep', o.kind === 'over');
  }

  function applyLang() {
    document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
    document.querySelectorAll('[data-i18n]').forEach((el) => {
      el.innerHTML = t(el.dataset.i18n);
    });
    langBtn.textContent = lang === 'zh' ? 'EN' : '中';
    langBtn.title = t('langTitle');
    applyTheme();
    applySpeed(false);
    renderStatus();
    renderOverlay();
  }

  /* ------------------------------------------------ layout */
  let lastCell = -1;
  let lastGap = -1;
  let layoutRaf = 0;

  function layout() {
    const w = boardEl.clientWidth;
    if (!w) return;
    const gap = w < 360 ? 8 : w < 430 ? 10 : 12;
    const cell = (w - 5 * gap) / 4;
    // Cached for pointermove: reading clientWidth from a movement handler forces
    // a synchronous layout on every sampled touch point.
    swipePx = Math.max(12, Math.min(26, w * 0.038));
    // A custom property on the board invalidates style for all sixteen cells
    // and every tile below it, so only write when the numbers really changed.
    if (Math.abs(cell - lastCell) < 0.5 && gap === lastGap) return;
    lastCell = cell;
    lastGap = gap;
    boardEl.style.setProperty('--gap', gap + 'px');
    boardEl.style.setProperty('--cell', cell + 'px');
  }

  // At most one geometry write per frame: a window drag fires the observer more
  // often than the compositor paints.
  function scheduleLayout() {
    if (layoutRaf) return;
    layoutRaf = requestAnimationFrame(() => {
      layoutRaf = 0;
      layout();
    });
  }

  // Dragging a window edge resizes the board every frame, and every tile, its
  // gradient and its shadow has to be rasterised again. Shadows and the idle
  // glow animation are dropped for the duration of the drag and restored right
  // after it - geometry stays exact, and nothing looks different once the drag
  // stops.
  let lastWidth = -1;
  let resizeTimer = 0;

  function onBoardResize() {
    const w = boardEl.clientWidth;
    if (!w) return;
    if (lastWidth < 0) { lastWidth = w; return; }   // first observe is the boot layout
    if (Math.abs(w - lastWidth) < 0.5) return;
    lastWidth = w;

    const root = document.documentElement;
    root.classList.add('resizing');
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      root.classList.remove('resizing');
      layout();
    }, 170);

    scheduleLayout();
  }

  function buildGrid() {
    const frag = document.createDocumentFragment();
    for (let r = 0; r < SIZE; r++) {
      for (let c = 0; c < SIZE; c++) {
        const d = document.createElement('div');
        d.className = 'cell';
        d.style.setProperty('--r', r);
        d.style.setProperty('--c', c);
        frag.appendChild(d);
      }
    }
    gridEl.appendChild(frag);
  }

  /* ------------------------------------------------ tiles */
  function place(tile) {
    tile.el.style.setProperty('--r', tile.row);
    tile.el.style.setProperty('--c', tile.col);
  }

  function paint(tile, anim) {
    const v = tile.value;
    const digits = String(v);
    const inner = tile.inner;

    // Writing the same text and attributes again still costs a style pass on the
    // tile, and most repaints here land on an unchanged value.
    if (inner.dataset.v !== digits) {
      inner.dataset.v = v;
      inner.dataset.len = digits.length;
      // Long values wrap onto two tidy lines inside the tile instead of shrinking or breaking
      if (digits.length >= 7) {
        const mid = Math.ceil(digits.length / 2);
        inner.innerHTML = digits.slice(0, mid) + '<br>' + digits.slice(mid);
      } else {
        inner.textContent = digits;
      }
    }
    if (anim) {
      // A freshly inserted element runs its animation from the first style
      // calculation, so this never needs a forced reflow.
      inner.classList.remove('appear');
      inner.classList.add(anim);
    }
  }

  function createTile(row, col, value, anim) {
    const tile = { id: nextId++, row, col, value, mergedNow: false };
    const el = document.createElement('div');
    el.className = 'tile';
    const inner = document.createElement('div');
    inner.className = 'tile-inner';
    el.appendChild(inner);
    tilesEl.appendChild(el);
    tile.el = el;
    tile.inner = inner;
    place(tile);
    paint(tile, anim || null);
    tiles.set(tile.id, tile);
    cells[row][col] = tile;
    return tile;
  }

  const allTiles = () => Array.from(tiles.values());

  function clearTiles() {
    tilesEl.innerHTML = '';
    tiles.clear();
    cells = Array.from({ length: SIZE }, () => Array(SIZE).fill(null));
  }

  function values() {
    const out = [];
    for (let r = 0; r < SIZE; r++)
      for (let c = 0; c < SIZE; c++) out.push(cells[r][c] ? cells[r][c].value : 0);
    return out;
  }

  function maxOf(vals) {
    return vals.reduce((a, b) => Math.max(a, b), 0);
  }

  function emptyCells() {
    const out = [];
    for (let r = 0; r < SIZE; r++)
      for (let c = 0; c < SIZE; c++) if (!cells[r][c]) out.push([r, c]);
    return out;
  }

  function spawnRandom() {
    const empty = emptyCells();
    if (!empty.length) return null;
    const [r, c] = empty[Math.floor(Math.random() * empty.length)];
    return createTile(r, c, Math.random() < 0.9 ? 2 : 4, 'appear');
  }

  /* ------------------------------------------------ scorekeeping */
  function fmt(n) {
    return n.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }

  let bestTimer = 0;
  let popTimer = 0;

  function flushBest() {
    clearTimeout(bestTimer);
    bestTimer = 0;
    try { localStorage.setItem(STORE.best, String(best)); } catch { /* private mode */ }
  }

  function updateScore(pop) {
    scoreEl.textContent = fmt(score);
    if (score > best) {
      best = score;
      // A synchronous storage write on every move is a real hitch at eight moves
      // per second; coalesce them and flush on the way out.
      if (!bestTimer) bestTimer = setTimeout(flushBest, 400);
    }
    bestEl.textContent = fmt(best);

    // Walking the live tiles skips building a sixteen-element array per move.
    let maxV = 0;
    for (const tile of tiles.values()) if (tile.value > maxV) maxV = tile.value;
    ui.maxTile = maxV;
    renderStatus();

    if (pop && !scoreBox.classList.contains('pop')) {
      // Restarting the class animation would need a forced reflow on every move;
      // a pop that is already running is left alone instead.
      scoreBox.classList.add('pop');
      clearTimeout(popTimer);
      popTimer = setTimeout(() => scoreBox.classList.remove('pop'), 220);
    }
  }

  /* ------------------------------------------------ moves */
  const snapshot = () => ({ vals: values(), score });

  // The merge pop, expressed as a web animation instead of a CSS class. Same
  // shape as the old .pop keyframes, but animate() replaces a running animation
  // on its own, so no forced reflow is needed to restart it.
  const MERGE_POP = [
    { transform: 'scale(1)' },
    { transform: 'scale(1.19)', offset: 0.42 },
    { transform: 'scale(1)' },
  ];
  const MERGE_POP_TIMING = { duration: 150, easing: 'cubic-bezier(0.24, 0.9, 0.32, 1.3)' };

  // A move is settled in two stages: the tiles start sliding at once, and the
  // merge, the score and the new tile land a few dozen milliseconds later. That
  // delay is purely visual, so the input path must never wait for it - pressing
  // a direction while the previous slide is still on screen settles that slide
  // on the spot and plays the new move immediately. Before this every key press
  // inside that window was dropped, and that is what made the board feel laggy.
  function settleStep(step) {
    const s = step || pendingStep;
    if (!s || s.done) return;
    s.done = true;
    if (pendingStep === s) pendingStep = null;
    clearTimeout(s.timer);
    animating = false;
    if (s.token !== token) return;      // superseded by a restart or a restore

    for (const m of s.merges) {
      m.victim.el.remove();
      tiles.delete(m.victim.id);
    }
    for (const tile of tiles.values()) tile.mergedNow = false;
    // Value first, then the pop through the Web Animations API. Restarting a CSS
    // animation needs a forced reflow (void offsetWidth) per merged tile, and at
    // eight moves a second that reflow lands right when the next slide wants to
    // start. animate() replaces the running animation without touching style.
    for (const m of s.merges) {
      paint(m.target, null, false);
      m.target.inner.animate(MERGE_POP, MERGE_POP_TIMING);
    }

    score += s.gained;
    updateScore(true);
    spawnRandom();

    finishTurn();

    // A direction pressed while this slide was running waited in a single slot
    // instead of cutting the slide short. Play it the moment the move settles,
    // which is what makes fast human input feel like a queue rather than a
    // stutter: every slide is still drawn, none is skipped.
    if (queuedDir >= 0) {
      const next = queuedDir;
      queuedDir = -1;
      if (!gameOver) move(next);
    }
  }

  function cancelStep() {
    queuedDir = -1;
    if (!pendingStep) return;
    pendingStep.done = true;
    clearTimeout(pendingStep.timer);
    pendingStep = null;
    animating = false;
  }

  function restore(snap) {
    cancelStep();
    token++;
    clearTiles();
    score = snap.score;
    const v = snap.vals;
    for (let i = 0; i < 16; i++) {
      if (v[i]) createTile(Math.floor(i / 4), i % 4, v[i], null);
    }
    updateScore(false);
    hideOverlay();
    gameOver = false;
    ui.evalV = null;
    ui.ms = 0;
    setAi('idle', 'off');
    renderStatus();
  }

  function canMove() {
    for (let r = 0; r < SIZE; r++) {
      for (let c = 0; c < SIZE; c++) {
        const t0 = cells[r][c];
        if (!t0) return true;                 // A free cell always means some direction is playable
        if (c + 1 < SIZE) {
          const n = cells[r][c + 1];
          if (n && n.value === t0.value) return true;
        }
        if (r + 1 < SIZE) {
          const n = cells[r + 1][c];
          if (n && n.value === t0.value) return true;
        }
      }
    }
    return false;
  }

  function move(dir) {
    if (gameOver) return false;
    if (animating) settleStep();     // finish the previous slide now, do not drop the input
    if (gameOver) return false;      // settling it may have ended the game

    const [dr, dc] = VEC[dir];
    const rows = [0, 1, 2, 3];
    const cols = [0, 1, 2, 3];
    if (dr > 0) rows.reverse();
    if (dc > 0) cols.reverse();

    const snap = snapshot();
    const merges = [];
    let moved = false;
    let gained = 0;

    for (const r of rows) {
      for (const c of cols) {
        const tile = cells[r][c];
        if (!tile) continue;

        let cr = r, cc = c;
        for (;;) {
          const nr = cr + dr, nc = cc + dc;
          if (nr < 0 || nr >= SIZE || nc < 0 || nc >= SIZE || cells[nr][nc]) break;
          cr = nr; cc = nc;
        }

        const nr = cr + dr, nc = cc + dc;
        const nxt = (nr >= 0 && nr < SIZE && nc >= 0 && nc < SIZE) ? cells[nr][nc] : null;

        if (nxt && nxt.value === tile.value && !nxt.mergedNow) {
          cells[r][c] = null;
          tile.row = nr; tile.col = nc;
          nxt.value *= 2;
          nxt.mergedNow = true;
          gained += nxt.value;
          merges.push({ victim: tile, target: nxt });
          moved = true;
        } else if (cr !== r || cc !== c) {
          cells[r][c] = null;
          cells[cr][cc] = tile;
          tile.row = cr; tile.col = cc;
          moved = true;
        }
      }
    }

    if (!moved) return false;

    animating = true;
    // Autopilot runs at the speed the player picked; a human move always runs at
    // the readable pace, whatever the button says. The settle timer has to use
    // the same number as the CSS transition, or the next tile appears while the
    // previous slide is still moving.
    const slideMs = autoOn ? moveMsNow : HUMAN_MS;
    setSlideMs(slideMs);
    history.push(snap);
    if (history.length > 80) history.shift();

    for (const tile of tiles.values()) place(tile);

    const step = { token: ++token, merges, gained, done: false, timer: 0 };
    // The small buffer lets the browser commit the start positions before the
    // merge lands; without it a very short slide can be skipped entirely.
    step.timer = setTimeout(() => settleStep(step), slideMs + 10);
    pendingStep = step;

    return true;
  }

  function finishTurn() {
    let maxTile = 0;
    for (const tile of tiles.values()) if (tile.value > maxTile) maxTile = tile.value;
    saveState();

    if (maxTile >= 32768 && !reachedGoal) {
      reachedGoal = true;
      ui.overlay = { kind: 'win', max: maxTile, score };
      overlay.hidden = false;
      renderOverlay();
      return;
    }

    if (!canMove()) {
      gameOver = true;
      setAuto(false);
      ui.overlay = { kind: 'over', max: maxTile, score };
      overlay.hidden = false;
      renderOverlay();
      setAi('ended', 'off');
      return;
    }

    if (autoOn) setTimeout(() => { if (autoOn) askAI("auto"); }, 0);
  }

  /* ------------------------------------------------ overlay / toasts */
  function hideOverlay() {
    overlay.hidden = true;
    ui.overlay = null;
  }

  let toastTimer = 0;
  function toast(msg) {
    toastEl.textContent = msg;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove('show'), 1900);
  }

  let hintTimer = 0;
  function flashHint(dir) {
    const chev = hintEl.querySelector('.chev');
    chev.style.setProperty('--rot', DIR_ROT[dir] + 'deg');
    hintEl.classList.add('show');
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => hintEl.classList.remove('show'), 1100);
  }

  function setAi(key, dotCls) {
    ui.aiKey = key;
    ui.dotCls = dotCls;
    renderStatus();
  }

  /* ------------------------------------------------ AI bridge */
  function askAI(mode) {
    if (gameOver) return;
    if (animating) { setTimeout(() => { if (!animating) askAI(mode); }, 70); return; }
    if (!bridge()) {
      setAi('browserOnly', 'off');
      toast(t('needDesktop'));
      return;
    }
    const vals = values();
    if (vals.every((v) => v === 0)) return;
    reqId++;
    aiMode = mode;
    pendingSig = vals.join(',');
    setAi(mode === 'auto' ? 'thinking' : 'analyzing', 'busy');
    post('AI ' + reqId + ' ' + vals.join(' '));
  }

  function fmtScore(v) {
    if (!Number.isFinite(v)) return '—';
    const a = Math.abs(v);
    if (a >= 1e6) return (v / 1e6).toFixed(2) + 'M';
    if (a >= 1e3) return (v / 1e3).toFixed(1) + 'k';
    return v.toFixed(0);
  }

  window.__aiRes = function (id, dir, s0, s1, s2, s3, ms) {
    if (id !== reqId) return;
    if (values().join(',') !== pendingSig) return;

    ui.ms = ms;

    if (dir < 0) {
      ui.evalV = null;
      setAi('noMove', 'off');
      return;
    }

    const scores = [s0, s1, s2, s3];
    ui.evalV = Math.max.apply(null, scores);
    ui.dir = dir;
    setAi('suggest', 'on');

    if (aiMode === 'hint') {
      flashHint(dir);
      return;
    }

    if (!autoOn) return;

    if (aiMode === 'auto' && autoOn) {
      if (!attemptMove(dir, scores)) {
        stuckCounter++;
        if (stuckCounter > 3) { setAuto(false); toast(t('stuck')); }
        else setTimeout(() => { if (autoOn) askAI('auto'); }, 60);
        return;
      }
      stuckCounter = 0;
    }
  };

  /* ------------------------------------------------ autoplay */
  function setAuto(on) {
    autoOn = on;
    autoBtn.classList.toggle('on', on);
    if (on) {
      setAi('running', 'on');
      askAI('auto');
    } else {
      setAi('idle', 'off');
    }
  }

  /* ------------------------------------------------ input */
  // Human input is queued one deep rather than cutting the running slide short.
  // A press that arrives mid-slide waits for it and then plays; presses that
  // pile up past one slot collapse into the newest, so holding a key never
  // builds a backlog. Every slide is drawn, which is the rhythm the mature 2048
  // pages have and the reason fast play stops feeling jumpy.
  function doMove(dir) {
    if (gameOver) {
      if (!pendingRestart) return;
      startGame();               // 保留的最后盘面到此为止，这一手落在新局上
    }
    if (autoOn) setAuto(false);
    if (animating) { queuedDir = dir; return; }
    move(dir);
  }

  const KEYMAP = {
    ArrowUp: 0, ArrowRight: 1, ArrowDown: 2, ArrowLeft: 3,
    w: 0, d: 1, s: 2, a: 3, W: 0, D: 1, S: 2, A: 3,
  };

  window.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;

    if ((e.key === 'Enter' || e.key === ' ') && !overlay.hidden) {
      e.preventDefault();
      $('#ovBtn').click();
      return;
    }
    if (e.key === 'l' || e.key === 'L') { langBtn.click(); return; }
    if (e.key === 't' || e.key === 'T') { themeBtn.click(); return; }

    const dir = KEYMAP[e.key];
    if (dir === undefined) return;
    e.preventDefault();
    doMove(dir);
  });

  // Swipe: the move fires as soon as the finger has travelled far enough to name
  // a direction. The gesture is then re-armed at the current point instead of
  // being dropped, so a thumb that keeps travelling - or turns without lifting -
  // can fire the next move. Fast mobile play is exactly that: several moves
  // inside one press, which used to be impossible because only the first swipe
  // of a press counted. A short cooldown keeps one long drag from firing on every
  // sampled frame, and the travel is measured against a cached slop so
  // pointermove never touches layout.
  let drag = null;
  let swipePx = 18;
  let lastSwipeAt = 0;

  boardEl.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 && e.pointerType === 'mouse') return;
    if (e.target.closest('.overlay')) return;
    drag = { x: e.clientX, y: e.clientY, id: e.pointerId };
  });

  boardEl.addEventListener('pointermove', (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    const dx = e.clientX - drag.x;
    const dy = e.clientY - drag.y;
    const ax = Math.abs(dx), ay = Math.abs(dy);
    if (Math.max(ax, ay) < swipePx) return;
    const now = performance.now();
    if (now - lastSwipeAt < 70) return;   // still the same flick, not a new move
    lastSwipeAt = now;
    drag.x = e.clientX;
    drag.y = e.clientY;
    if (ax > ay) doMove(dx > 0 ? 1 : 3);
    else doMove(dy > 0 ? 2 : 0);
  });

  function endDrag() { drag = null; }
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', () => { drag = null; });
  boardEl.addEventListener('contextmenu', (e) => e.preventDefault());
  document.addEventListener('dragstart', (e) => e.preventDefault());

  /* ------------------------------------------------ buttons */
  // One restart control only: New Game already does exactly this, so the second
  // slot that undo used to hold is gone again.
  $('#newGame').addEventListener('click', () => startGame());

  autoBtn.addEventListener('click', () => {
    if (gameOver) {
      if (!pendingRestart) return;
      startGame();
      setAuto(true);
      return;
    }
    setAuto(!autoOn);
  });

  $('#hintBtn').addEventListener('click', () => {
    if (autoOn) setAuto(false);
    if (gameOver) {
      if (!pendingRestart) return;
      startGame();
    }
    askAI('hint');
  });

  // ?auto=1 starts the AI immediately, which doubles as the demo link. Wait for
  // the engine (bounded, so a broken pool still starts a playable game).
  if (/[?&]auto=1/.test(location.search)) {
    const go = () => { startGame(); setAuto(true); };
    const engine = window.LiuEngine;
    if (engine && !engine.ready) {
      Promise.race([
        engine.init(),
        new Promise((r) => setTimeout(r, 3000)),
      ]).then(() => setTimeout(go, 150));
    } else {
      setTimeout(go, 400);
    }
  }

  // Browser build: bring up the worker pool and say which engine is running.
  if (!nativeHost() && window.LiuEngine) {
    window.LiuEngine.init().then((n) => {
      const chip = $('#engineChip');
      if (chip) chip.textContent = 'WASM · ' + n + ' WORKERS';
      setAi('ready', 'off');
    }).catch((err) => {
      console.error('engine init failed', err);
      const chip = $('#engineChip');
      if (chip) chip.textContent = 'ENGINE FAILED: ' + (window.LiuEngine.lastError || err);
    });
  }

  overlay.addEventListener('pointerdown', (e) => e.stopPropagation());

  // Dismiss only. The finished board is what the player just achieved, so the
  // overlay closes over it instead of throwing it away. After a game over the
  // board stays exactly as it ended; the next direction key (or Restart) begins
  // a new game.
  $('#ovBtn').addEventListener('click', () => {
    const kind = ui.overlay ? ui.overlay.kind : null;
    hideOverlay();
    if (kind === 'over') {
      pendingRestart = true;
      toast(t('kept'));
    }
  });

  langBtn.addEventListener('click', () => {
    lang = lang === 'zh' ? 'en' : 'zh';
    localStorage.setItem(STORE.lang, lang);
    applyLang();
  });

  speedBtn.addEventListener('click', () => {
    speedIdx = (speedIdx + 1) % SPEEDS.length;
    applySpeed(true);
    toast(t('speedTitle') + ' ' + SPEEDS[speedIdx] + 'x');
  });

  themeBtn.addEventListener('click', () => {
    theme = theme === 'dark' ? 'light' : 'dark';
    localStorage.setItem(STORE.theme, theme);
    applyTheme();
  });

  /* ------------------------------------------------ persistence */
  // The board, the score and whether the game had ended are written after every
  // settled move, so closing the tab or the app and coming back shows exactly the
  // position that was on screen - not a fresh game. The write itself is
  // coalesced: serialising and storing sixteen numbers eight times a second is
  // measurable main-thread time, and only the last position matters.
  let saveTimer = 0;

  function flushState() {
    clearTimeout(saveTimer);
    saveTimer = 0;
    try {
      localStorage.setItem(STORE.save, JSON.stringify({
        vals: values(), score, gameOver, reachedGoal,
      }));
    } catch {
      /* private mode: keep playing, just without a saved game */
    }
  }

  function saveState() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(flushState, 250);
  }

  window.addEventListener('pagehide', flushState);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') { flushState(); flushBest(); }
  });

  function restoreSaved() {
    let saved = null;
    try {
      saved = JSON.parse(localStorage.getItem(STORE.save) || 'null');
    } catch {
      return false;
    }
    if (!saved || !Array.isArray(saved.vals) || saved.vals.length !== 16) return false;
    if (!saved.vals.some((v) => v > 0)) return false;
    restore({ vals: saved.vals, score: saved.score | 0 });
    gameOver = !!saved.gameOver;
    reachedGoal = !!saved.reachedGoal;
    if (gameOver) {
      ui.overlay = { kind: 'over', max: maxOf(values()), score };
      overlay.hidden = false;
      renderOverlay();
      setAi('ended', 'off');
    }
    return true;
  }

  /* ------------------------------------------------ startup */
  function startGame() {
    post('NEW');
    // The browser pool keeps its transposition tables across games; reset them
    // here so a new game starts from the same engine state as a fresh page.
    if (window.LiuEngine && window.LiuEngine.reset) window.LiuEngine.reset();
    cancelStep();
    token++;
    gameOver = false;
    pendingRestart = false;
    reachedGoal = false;
    history = [];
    score = 0;
    clearTiles();
    hideOverlay();
    setAuto(false);
    ui.evalV = null;
    ui.ms = 0;
    ui.dir = null;
    spawnRandom();
    spawnRandom();
    updateScore(false);
    flushState();
  }

  // The browser build starts at 4x: the search now answers in about 27 ms, and at
  // 1x the board animation, not the engine, sets the pace. A stored preference
  // still wins.
  const savedSpeed = localStorage.getItem(STORE.speed);
  speedIdx = savedSpeed === null
    ? 2
    : Math.min(SPEEDS.length - 1, Math.max(0, Number(savedSpeed) || 0));
  applySpeed(false);

  buildGrid();
  layout();
  if (!restoreSaved()) startGame();
  new ResizeObserver(onBoardResize).observe(boardEl);
  applyLang();
  if (!bridge()) setAi('browserOnly', 'off');
})();

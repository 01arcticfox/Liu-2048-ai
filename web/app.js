/* ============================================================
   2048 AI · Blue —— 游戏引擎 + 动画 + AI 桥接 + 中英双语 + 双主题
   ============================================================ */
(() => {
  'use strict';

  const SIZE = 4;
  const VEC = { 0: [-1, 0], 1: [0, 1], 2: [1, 0], 3: [0, -1] }; // 0 up, 1 right, 2 down, 3 left
  const DIR_GLYPH = { 0: '↑', 1: '→', 2: '↓', 3: '←' };
  const DIR_ROT = { 0: -90, 1: 0, 2: 90, 3: 180 };
  const STORE = { best: 'b2048.best', lang: 'b2048.lang', theme: 'b2048.theme', speed: 'b2048.speed' };

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

  /* ------------------------------------------------ i18n */
  const DICT = {
    zh: {
      sub: 'Bitboard 期望搜索 · 目标 2<sup>15</sup> = 32768',
      score: '得分', best: '最高',
      newGame: '新游戏', auto: 'AI 自动', hint: 'AI 提示', undo: '撤销',
      tips: 'WASD / 方向键 · 鼠标拖拽 · 触屏滑动',
      idle: 'AI 待机', thinking: 'AI 思考中…', analyzing: 'AI 分析中…', running: 'AI 自动运行',
      suggest: '建议', evalNone: '评估 —', evalWord: '评估', maxPrefix: '最大',
      noMove: '无可行方向', ended: '已结束', browserOnly: '浏览器预览模式',
      needDesktop: 'AI 引擎需要桌面版（WebView2）运行',
      noUndo: '没有可撤销的步骤', undone: '已撤销', stuck: 'AI 无法继续',
      over: 'GAME OVER', again: '再来一局',
      overText: (m, s) => `最大方块 ${m} · 得分 ${s}`,
      win: '32768 达成', winBtn: '继续挑战',
      winText: '已经拼出 2¹⁵，继续冲击 2¹⁶？',
      toLight: '切换浅色主题', toDark: '切换深色主题', langTitle: 'Switch to English',
      speedTitle: 'AI 速度',
    },
    en: {
      sub: 'Bitboard expectimax · target 2<sup>15</sup> = 32768',
      score: 'Score', best: 'Best',
      newGame: 'New Game', auto: 'Auto AI', hint: 'Hint', undo: 'Undo',
      tips: 'WASD / Arrows · Drag · Swipe',
      idle: 'AI idle', thinking: 'Thinking…', analyzing: 'Analyzing…', running: 'Auto running',
      suggest: 'Suggest', evalNone: 'Eval —', evalWord: 'Eval', maxPrefix: 'Max',
      noMove: 'No moves', ended: 'Finished', browserOnly: 'Browser preview',
      needDesktop: 'AI engine runs in the desktop app (WebView2)',
      noUndo: 'Nothing to undo', undone: 'Undone', stuck: 'AI stalled',
      over: 'GAME OVER', again: 'Play Again',
      overText: (m, s) => `Max tile ${m} · Score ${s}`,
      win: '32768 Reached', winBtn: 'Keep going',
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
  let token = 0;
  let gameOver = false;
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

  function applySpeed(save) {
    const ms = Math.max(12, Math.round(130 / SPEEDS[speedIdx]));
    document.documentElement.style.setProperty('--move-ms', ms + 'ms');
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

  function moveMs() {
    const v = getComputedStyle(document.documentElement).getPropertyValue('--move-ms');
    const n = parseInt(v, 10);
    return Number.isFinite(n) ? n : 130;
  }

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
  function layout() {
    const w = boardEl.clientWidth;
    if (!w) return;
    const gap = w < 360 ? 8 : w < 430 ? 10 : 12;
    const cell = (w - 5 * gap) / 4;
    boardEl.classList.add('no-anim');
    boardEl.style.setProperty('--gap', gap + 'px');
    boardEl.style.setProperty('--cell', cell + 'px');
    requestAnimationFrame(() => boardEl.classList.remove('no-anim'));
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
    tile.inner.dataset.v = v;
    tile.inner.dataset.len = digits.length;

    // Long values wrap onto two tidy lines inside the tile instead of shrinking or breaking
    if (digits.length >= 7) {
      const mid = Math.ceil(digits.length / 2);
      tile.inner.innerHTML = digits.slice(0, mid) + '<br>' + digits.slice(mid);
    } else {
      tile.inner.textContent = digits;
    }
    if (anim) {
      tile.inner.classList.remove('appear', 'pop');
      void tile.inner.offsetWidth;
      tile.inner.classList.add(anim);
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

  function updateScore(pop) {
    scoreEl.textContent = fmt(score);
    if (score > best) {
      best = score;
      localStorage.setItem(STORE.best, String(best));
    }
    bestEl.textContent = fmt(best);
    ui.maxTile = maxOf(values());
    renderStatus();
    if (pop) {
      const box = $('#scoreBox');
      box.classList.remove('pop');
      void box.offsetWidth;
      box.classList.add('pop');
      setTimeout(() => box.classList.remove('pop'), 220);
    }
  }

  /* ------------------------------------------------ moves */
  const snapshot = () => ({ vals: values(), score });

  function restore(snap) {
    token++;
    animating = false;
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
    if (gameOver || animating) return false;

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
    history.push(snap);
    if (history.length > 80) history.shift();

    for (const tile of allTiles()) place(tile);

    const my = ++token;
    const ms = moveMs();

    setTimeout(() => {
      if (my !== token) return;

      for (const m of merges) {
        m.victim.el.remove();
        tiles.delete(m.victim.id);
      }
      for (const tile of allTiles()) tile.mergedNow = false;
      for (const m of merges) paint(m.target, 'pop');

      score += gained;
      updateScore(true);
      spawnRandom();

      animating = false;
      finishTurn();
    }, ms + 16);

    return true;
  }

  function finishTurn() {
    const vals = values();
    const maxTile = maxOf(vals);

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

    if (autoOn) setTimeout(() => { if (autoOn) askAI('auto'); }, 20);
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
  // A human move animates a little slower than the AI's own pace: at 8x the
  // tiles are a blur, which is fine when the machine is playing and confusing
  // when you are. The AI path keeps whatever speed the player picked.
  function doMove(dir) {
    if (gameOver) return;
    const wasAuto = autoOn;
    if (autoOn) setAuto(false);
    if (!wasAuto) {
      const root = document.documentElement;
      const fast = Math.max(12, Math.round(130 / SPEEDS[speedIdx]));
      const slow = Math.max(110, Math.round(fast * 1.35));
      root.style.setProperty('--move-ms', slow + 'ms');
      setTimeout(() => {
        root.style.setProperty('--move-ms', Math.max(12, Math.round(130 / SPEEDS[speedIdx])) + 'ms');
      }, slow + 40);
    }
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

  // Swipe: as soon as the finger (or the mouse) has travelled far enough to name
  // a direction, the move fires - no need to lift. The drag is then dropped, so
  // one gesture produces exactly one move however long the finger stays down.
  let drag = null;
  const swipeSlop = () => Math.max(14, Math.min(30, boardEl.clientWidth * 0.045));

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
    if (Math.max(ax, ay) < swipeSlop()) return;
    drag = null;
    if (ax > ay) doMove(dx > 0 ? 1 : 3);
    else doMove(dy > 0 ? 2 : 0);
  });

  function endDrag() { drag = null; }
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', () => { drag = null; });
  boardEl.addEventListener('contextmenu', (e) => e.preventDefault());
  document.addEventListener('dragstart', (e) => e.preventDefault());

  /* ------------------------------------------------ buttons */
  $('#newGame').addEventListener('click', () => startGame());

  autoBtn.addEventListener('click', () => {
    if (gameOver) return;
    setAuto(!autoOn);
  });

  $('#hintBtn').addEventListener('click', () => {
    if (autoOn) setAuto(false);
    askAI('hint');
  });

  // ?auto=1 starts the AI immediately, which doubles as the demo link.
  if (/[?&]auto=1/.test(location.search)) {
    setTimeout(() => { startGame(); setAuto(true); }, 400);
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

  $('#undoBtn').addEventListener('click', () => {
    if (!history.length) { toast(t('noUndo')); return; }
    if (autoOn) setAuto(false);
    restore(history.pop());
    toast(t('undone'));
  });

  overlay.addEventListener('pointerdown', (e) => e.stopPropagation());

  $('#ovBtn').addEventListener('click', () => {
    if (gameOver) { startGame(); return; }
    hideOverlay();
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

  /* ------------------------------------------------ startup */
  function startGame() {
    post('NEW');
    token++;
    animating = false;
    gameOver = false;
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
  }

  speedIdx = Math.min(SPEEDS.length - 1, Math.max(0, Number(localStorage.getItem(STORE.speed) || 0)));
  applySpeed(false);

  buildGrid();
  layout();
  startGame();
  new ResizeObserver(layout).observe(boardEl);
  applyLang();
  if (!bridge()) setAi('browserOnly', 'off');
  if (qs.get('auto') === '1') setTimeout(() => setAuto(true), 300);
})();

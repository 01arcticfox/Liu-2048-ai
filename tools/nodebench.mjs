// Headless benchmark for the WebAssembly search core.
//
// Plays full games with the same evaluation and move selection the browser
// build uses, so the numbers in the README come from real play rather than a
// node-rate estimate.
//
// usage: node nodebench.mjs [--games N] [--target T] [--depth D] [--budget B]

import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = join(here, '..', 'web', 'wasm', 'solver.wasm');

const args = process.argv.slice(2);
function arg(name, def) {
  const i = args.indexOf('--' + name);
  return i >= 0 ? Number(args[i + 1]) : def;
}
const GAMES = arg('games', 20);
const TARGET = arg('target', 32768);
const DEPTH = arg('depth', 5);
const BUDGET = arg('budget', 0);   // > 0: nodes per move, overrides depth

const wasm = await WebAssembly.instantiate(await readFile(wasmPath), {
  env: {
    abort(msg, file, line, col) {
      throw new Error(`wasm abort at ${line}:${col}`);
    },
  },
});
const instance = wasm.instance;
const W = instance.exports;
W.init();

// --- game side tables (slide left/right per 16-bit line) ---------------------
const leftOf = new Int32Array(65536);
const gainL = new Float32Array(65536);
const rightOf = new Int32Array(65536);
const gainR = new Float32Array(65536);

function nib(line, i) { return (line >>> (i * 4)) & 0xF; }
{
  const cells = [];
  for (let line = 0; line < 65536; line++) {
    cells.length = 0;
    for (let i = 0; i < 4; i++) { const v = nib(line, i); if (v) cells.push(v); }
    const out = [];
    let gain = 0;
    for (let i = 0; i < cells.length;) {
      if (i + 1 < cells.length && cells[i] === cells[i + 1]) {
        out.push(cells[i] + 1);
        gain += 1 << (cells[i] + 1);
        i += 2;
      } else { out.push(cells[i]); i += 1; }
    }
    let packed = 0;
    for (let k = 0; k < out.length; k++) packed |= out[k] << (k * 4);
    leftOf[line] = packed;
    gainL[line] = gain;

    let mirrored = 0;
    for (let k = 0; k < 4; k++) mirrored |= nib(line, k) << ((3 - k) * 4);
    const ml = leftOf[mirrored];
    let back = 0;
    for (let k = 0; k < 4; k++) back |= nib(ml, k) << ((3 - k) * 4);
    rightOf[line] = back;
    gainR[line] = gainL[mirrored];
  }
}

function transpose(a) {
  const m = 0xF;
  return [
    (a[0] & m) | ((a[1] & m) << 4) | ((a[2] & m) << 8) | ((a[3] & m) << 12),
    ((a[0] >> 4) & m) | (((a[1] >> 4) & m) << 4) | (((a[2] >> 4) & m) << 8) | (((a[3] >> 4) & m) << 12),
    ((a[0] >> 8) & m) | (((a[1] >> 8) & m) << 4) | (((a[2] >> 8) & m) << 8) | (((a[3] >> 8) & m) << 12),
    ((a[0] >> 12) & m) | (((a[1] >> 12) & m) << 4) | (((a[2] >> 12) & m) << 8) | (((a[3] >> 12) & m) << 12),
  ];
}

function moveRef(rows, dir) {
  const g = W.doMove(rows[0], rows[1], rows[2], rows[3], dir);
  if (g < 0) return null;
  return { rows: [W.outR0(), W.outR1(), W.outR2(), W.outR3()], gain: g };
}

function move(rows, dir) {
  let out, gain = 0;
  if (dir === 3) {
    out = [leftOf[rows[0]], leftOf[rows[1]], leftOf[rows[2]], leftOf[rows[3]]];
    gain = gainL[rows[0]] + gainL[rows[1]] + gainL[rows[2]] + gainL[rows[3]];
  } else if (dir === 1) {
    out = [rightOf[rows[0]], rightOf[rows[1]], rightOf[rows[2]], rightOf[rows[3]]];
    gain = gainR[rows[0]] + gainR[rows[1]] + gainR[rows[2]] + gainR[rows[3]];
  } else {
    const c = transpose(rows);
    if (dir === 0) {
      out = transpose([leftOf[c[0]], leftOf[c[1]], leftOf[c[2]], leftOf[c[3]]]);
      gain = gainL[c[0]] + gainL[c[1]] + gainL[c[2]] + gainL[c[3]];
    } else {
      out = transpose([rightOf[c[0]], rightOf[c[1]], rightOf[c[2]], rightOf[c[3]]]);
      gain = gainR[c[0]] + gainR[c[1]] + gainR[c[2]] + gainR[c[3]];
    }
  }
  const same = out[0] === rows[0] && out[1] === rows[1] && out[2] === rows[2] && out[3] === rows[3];
  return same ? null : { rows: out, gain };
}

function empties(rows) {
  const list = [];
  for (let cell = 0; cell < 16; cell++) {
    const r = cell >> 2, sh = (cell & 3) * 4;
    if (((rows[r] >>> sh) & 0xF) === 0) list.push(cell);
  }
  return list;
}

function spawn(rows, cell, rank) {
  const out = rows.slice();
  out[cell >> 2] |= rank << ((cell & 3) * 4);
  return out;
}

let rngState = Number(arg('seed', 12345)) || 12345;
function rnd() {
  rngState ^= rngState << 13; rngState >>>= 0;
  rngState ^= rngState >>> 17;
  rngState ^= rngState << 5; rngState >>>= 0;
  return rngState / 4294967296;
}

function bestMove(rows, depth) {
  let best = -1, bestVal = -Infinity;
  for (let d = 0; d < 4; d++) {
    const n = W.freeAfterMove(rows[0], rows[1], rows[2], rows[3], d);
    if (n === 0) continue;
    let v = 0;
    for (let s = 0; s < n; s++) {
      v += W.scoreShard(rows[0], rows[1], rows[2], rows[3], d, s, n, depth);
    }
    if (v > bestVal) { bestVal = v; best = d; }
  }
  return best;
}

function maxTile(rows) {
  let m = 0;
  for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) m = Math.max(m, (rows[r] >>> (c * 4)) & 0xF);
  return 1 << m;
}

// --------------------------------------------------------------------- games
let hits = 0, sumScore = 0, sumMoves = 0, sumNodes = 0;
const t0 = process.hrtime.bigint();

for (let g = 0; g < GAMES; g++) {
  let rows = [0, 0, 0, 0];
  for (let k = 0; k < 2; k++) {
    const e = empties(rows);
    rows = spawn(rows, e[Math.floor(rnd() * e.length)], rnd() < 0.9 ? 1 : 2);
  }
  let score = 0, moves = 0, best = 0;
  for (;;) {
    if (BUDGET > 0) W.resetNodes();
    const d = bestMove(rows, DEPTH);
    if (BUDGET > 0) sumNodes += Number(W.nodeCount());
    if (d < 0) break;
    const mv = moveRef(rows, d);
    if (!mv) break;
    rows = mv.rows; score += mv.gain; moves++;
    W.clearTT();
    const e = empties(rows);
    if (e.length === 0) {
      const t = maxTile(rows);
      if (t >= TARGET) { best = t; break; }
      // no spawn cell: the game is over
      best = t; break;
    }
    rows = spawn(rows, e[Math.floor(rnd() * e.length)], rnd() < 0.9 ? 1 : 2);
    best = maxTile(rows);
    if (best >= TARGET) break;
    if (moves > 200000) break;
  }
  if (best >= TARGET) hits++;
  sumScore += score; sumMoves += moves;
  process.stdout.write(`game ${g + 1}/${GAMES}  max ${best}  score ${score}  moves ${moves}\n`);
}

const ms = Number(process.hrtime.bigint() - t0) / 1e6;
console.log(`\nRESULT games ${GAMES}  hit ${hits} (${(100 * hits / GAMES).toFixed(0)}%)  ` +
  `score avg ${Math.round(sumScore / GAMES)}  moves avg ${Math.round(sumMoves / GAMES)}  ` +
  `wall ${(ms / 1000).toFixed(1)}s  ${(sumMoves / (ms / 1000)).toFixed(1)} moves/s` +
  (BUDGET > 0 ? `  nodes/move ${Math.round(sumNodes / Math.max(1, sumMoves))}` : ''));

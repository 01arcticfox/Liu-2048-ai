// 2048 search core, compiled to WebAssembly with AssemblyScript.
//
// Same algorithm as the native engine: expectimax over the spawn distribution,
// a per-line heuristic table, and a transposition table that accepts entries
// searched at least as deep as the request. Boards are four 16-bit rows, each
// row holding four 4-bit tile ranks, so a move is four table lookups.
//
// ---------------------------------------------------------------- constants

const ROWS: i32 = 4;
const LINE_COUNT: i32 = 65536;
// Transposition table. The size is picked at boot from the device: 2^19 entries
// costs 6 MB per worker and was worth about 20% of the search time in the
// benchmark, which is too much for a phone, so small devices keep 2^17.
let ttBits: i32 = 17;
let ttSize: i32 = 1 << ttBits;
let ttMask: i32 = ttSize - 1;

// Tile ranks are 0 (empty) .. 15 (32768). The heuristic works on tile values.
let powSum: StaticArray<f32> = new StaticArray<f32>(0);    // rank ^ sumExp
let powMono: StaticArray<f32> = new StaticArray<f32>(0);   // rank ^ monoExp

// Move tables: one entry per 16-bit line.
let rowLeft: StaticArray<i32> = new StaticArray<i32>(0);   // line after sliding left
let rowRight: StaticArray<i32> = new StaticArray<i32>(0);  // line after sliding right
let gainLeft: StaticArray<f32> = new StaticArray<f32>(0);  // score gained sliding left
let gainRight: StaticArray<f32> = new StaticArray<f32>(0);
let lineScore: StaticArray<f32> = new StaticArray<f32>(0); // heuristic value of a line

// Transposition table.
let ttKey: StaticArray<i32> = new StaticArray<i32>(0);
let ttVal: StaticArray<f32> = new StaticArray<f32>(0);
let ttDep: StaticArray<i32> = new StaticArray<i32>(0);

// Heuristic weights (kept identical to the native build).
let wEmpty: f32 = 270.0;
let wMerges: f32 = 700.0;
let wMono: f32 = 47.0;
let wSum: f32 = 11.0;
let sumExp: f32 = 3.5;
let monoExp: f32 = 4.0;

// Scratch rows written by the transpose helper.
let t0: i32 = 0;
let t1: i32 = 0;
let t2: i32 = 0;
let t3: i32 = 0;

let nodes: i64 = 0;

// ---------------------------------------------------------------- tables

@inline function nib(line: i32, idx: i32): i32 {
  return (line >> (idx * 4)) & 0xF;
}

function buildMoveTables(): void {
  rowLeft = new StaticArray<i32>(LINE_COUNT);
  rowRight = new StaticArray<i32>(LINE_COUNT);
  gainLeft = new StaticArray<f32>(LINE_COUNT);
  gainRight = new StaticArray<f32>(LINE_COUNT);

  const cells = new StaticArray<i32>(4);
  const out = new StaticArray<i32>(4);
  for (let line = 0; line < LINE_COUNT; ++line) {
    // slide left
    let n = 0;
    for (let i = 0; i < 4; ++i) {
      const v = nib(line, i);
      if (v != 0) { cells[n] = v; n += 1; }
    }
    let w = 0;
    let gain: f32 = 0;
    let i = 0;
    while (i < n) {
      if (i + 1 < n && cells[i] == cells[i + 1]) {
        const merged = cells[i] + 1;
        out[w] = merged;
        w += 1;
        // The score of merging two tiles of rank r is the value of rank r+1.
        gain += f32(1 << merged);
        i += 2;
      } else {
        out[w] = cells[i];
        w += 1;
        i += 1;
      }
    }
    let packed = 0;
    for (let k = 0; k < w; ++k) packed |= out[k] << (k * 4);
    rowLeft[line] = packed;
    gainLeft[line] = gain;
  }

  // Slide right needs the left table for the mirrored line, so it takes a second
  // pass: filling both in one loop would read entries that are not built yet.
  for (let line = 0; line < LINE_COUNT; ++line) {
    let mirrored = 0;
    for (let k = 0; k < 4; ++k) mirrored |= nib(line, k) << ((3 - k) * 4);
    const ml = rowLeft[mirrored];
    let back = 0;
    for (let k = 0; k < 4; ++k) back |= nib(ml, k) << ((3 - k) * 4);
    rowRight[line] = back;
    gainRight[line] = gainLeft[mirrored];
  }
  // free the temporaries; the arrays above are module level
}

function buildPowTables(): void {
  powSum = new StaticArray<f32>(16);
  powMono = new StaticArray<f32>(16);
  powSum[0] = 0; powMono[0] = 0;
  for (let r = 1; r < 16; ++r) {
    const f = f32(r);
    powSum[r] = f32(Math.pow(f64(f), f64(sumExp)));
    powMono[r] = f32(Math.pow(f64(f), f64(monoExp)));
  }
}

function buildLineScore(): void {
  lineScore = new StaticArray<f32>(LINE_COUNT);
  for (let line = 0; line < LINE_COUNT; ++line) {
    let empty = 0;
    let merges = 0;
    let prev = 0;
    let counter = 0;
    let sum: f32 = 0;
    for (let i = 0; i < 4; ++i) {
      const rank = nib(line, i);
      sum += powSum[rank];
      if (rank == 0) {
        empty += 1;
      } else {
        if (prev == rank) {
          counter += 1;
        } else if (counter > 0) {
          merges += 1 + counter;
          counter = 0;
        }
        prev = rank;
      }
    }
    if (counter > 0) merges += 1 + counter;

    let monoLeft: f32 = 0;
    let monoRight: f32 = 0;
    for (let i = 1; i < 4; ++i) {
      const a = nib(line, i - 1);
      const b = nib(line, i);
      if (a > b) monoLeft += powMono[a] - powMono[b];
      else monoRight += powMono[b] - powMono[a];
    }
    const mono = monoLeft < monoRight ? monoLeft : monoRight;
    lineScore[line] = 200000.0
      + wEmpty * f32(empty)
      + wMerges * f32(merges)
      - wMono * mono
      - wSum * sum;
  }
}

export function init(): void {
  buildPowTables();
  buildMoveTables();
  buildLineScore();
  allocTT(ttBits);
}

function allocTT(bits: i32): void {
  ttBits = bits < 12 ? 12 : bits > 21 ? 21 : bits;
  ttSize = 1 << ttBits;
  ttMask = ttSize - 1;
  ttKey = new StaticArray<i32>(ttSize);
  ttVal = new StaticArray<f32>(ttSize);
  ttDep = new StaticArray<i32>(ttSize);
}

// Called before the first search; the pool asks for a table sized to the device.
export function setTTBits(bits: i32): void { allocTT(bits); }

export function setWeights(e: f32, m: f32, mo: f32, s: f32): void {
  wEmpty = e; wMerges = m; wMono = mo; wSum = s;
  buildLineScore();
}

export function clearTT(): void {
  for (let i = 0; i < ttSize; ++i) ttDep[i] = 0;
}

export function nodeCount(): i64 { return nodes; }
export function resetNodes(): void { nodes = 0; }

// ---------------------------------------------------------------- board ops

@inline function transpose(a0: i32, a1: i32, a2: i32, a3: i32): void {
  const m: i32 = 0xF;
  t0 = (a0 & m) | ((a1 & m) << 4) | ((a2 & m) << 8) | ((a3 & m) << 12);
  t1 = ((a0 >> 4) & m) | (((a1 >> 4) & m) << 4) | (((a2 >> 4) & m) << 8) | (((a3 >> 4) & m) << 12);
  t2 = ((a0 >> 8) & m) | (((a1 >> 8) & m) << 4) | (((a2 >> 8) & m) << 8) | (((a3 >> 8) & m) << 12);
  t3 = ((a0 >> 12) & m) | (((a1 >> 12) & m) << 4) | (((a2 >> 12) & m) << 8) | (((a3 >> 12) & m) << 12);
}

// Writes the four rows of the board after moving in `dir` into t0..t3 and
// returns the score gained, or -1 when the move changes nothing.
// dir: 0 up, 1 right, 2 down, 3 left
@inline function applyMove(r0: i32, r1: i32, r2: i32, r3: i32, dir: i32): f32 {
  let a0 = 0, a1 = 0, a2 = 0, a3 = 0;
  let gain: f32 = 0;
  if (dir == 3 || dir == 1) {
    if (dir == 3) {
      a0 = rowLeft[r0]; a1 = rowLeft[r1]; a2 = rowLeft[r2]; a3 = rowLeft[r3];
      gain = gainLeft[r0] + gainLeft[r1] + gainLeft[r2] + gainLeft[r3];
    } else {
      a0 = rowRight[r0]; a1 = rowRight[r1]; a2 = rowRight[r2]; a3 = rowRight[r3];
      gain = gainRight[r0] + gainRight[r1] + gainRight[r2] + gainRight[r3];
    }
  } else {
    transpose(r0, r1, r2, r3);
    const c0 = t0, c1 = t1, c2 = t2, c3 = t3;
    let m0 = 0, m1 = 0, m2 = 0, m3 = 0;
    if (dir == 0) {
      m0 = rowLeft[c0]; m1 = rowLeft[c1]; m2 = rowLeft[c2]; m3 = rowLeft[c3];
      gain = gainLeft[c0] + gainLeft[c1] + gainLeft[c2] + gainLeft[c3];
    } else {
      m0 = rowRight[c0]; m1 = rowRight[c1]; m2 = rowRight[c2]; m3 = rowRight[c3];
      gain = gainRight[c0] + gainRight[c1] + gainRight[c2] + gainRight[c3];
    }
    transpose(m0, m1, m2, m3);
    a0 = t0; a1 = t1; a2 = t2; a3 = t3;
  }
  if (a0 == r0 && a1 == r1 && a2 == r2 && a3 == r3) return -1.0;
  t0 = a0; t1 = a1; t2 = a2; t3 = a3;
  return gain;
}

@inline function evalRows(r0: i32, r1: i32, r2: i32, r3: i32): f32 {
  return lineScore[r0] + lineScore[r1] + lineScore[r2] + lineScore[r3];
}

@inline function evalBoard(r0: i32, r1: i32, r2: i32, r3: i32): f32 {
  const rows = evalRows(r0, r1, r2, r3);
  transpose(r0, r1, r2, r3);
  return rows + lineScore[t0] + lineScore[t1] + lineScore[t2] + lineScore[t3];
}

@inline function hash4(r0: i32, r1: i32, r2: i32, r3: i32): i32 {
  let h: i32 = r0 * 73856093;
  h ^= r1 * 19349663;
  h ^= r2 * 83492791;
  h ^= r3 * 50331653;
  h = h ^ (h >> 13);
  return h;
}

// ---------------------------------------------------------------- search

function maxNode(r0: i32, r1: i32, r2: i32, r3: i32, depth: i32): f32 {
  nodes += 1;
  let best: f32 = 0;
  for (let d = 0; d < 4; ++d) {
    const g = applyMove(r0, r1, r2, r3, d);
    if (g < 0) continue;
    const v = expectNode(t0, t1, t2, t3, depth - 1);
    if (v > best) best = v;
  }
  return best;
}

function expectNode(r0: i32, r1: i32, r2: i32, r3: i32, depth: i32): f32 {
  nodes += 1;
  if (depth <= 0) return evalBoard(r0, r1, r2, r3);

  const h = hash4(r0, r1, r2, r3);
  const slot = h & ttMask;
  if (ttDep[slot] >= depth && ttKey[slot] == h) return ttVal[slot];

  // Count empty cells and the spawn weights.
  let n = 0;
  let mask: i32 = 0;
  if ((r0 & 0xF) == 0) { mask |= 1; n += 1; }
  if ((r0 & 0xF0) == 0) { mask |= 2; n += 1; }
  if ((r0 & 0xF00) == 0) { mask |= 4; n += 1; }
  if ((r0 & 0xF000) == 0) { mask |= 8; n += 1; }
  if ((r1 & 0xF) == 0) { mask |= 16; n += 1; }
  if ((r1 & 0xF0) == 0) { mask |= 32; n += 1; }
  if ((r1 & 0xF00) == 0) { mask |= 64; n += 1; }
  if ((r1 & 0xF000) == 0) { mask |= 128; n += 1; }
  if ((r2 & 0xF) == 0) { mask |= 256; n += 1; }
  if ((r2 & 0xF0) == 0) { mask |= 512; n += 1; }
  if ((r2 & 0xF00) == 0) { mask |= 1024; n += 1; }
  if ((r2 & 0xF000) == 0) { mask |= 2048; n += 1; }
  if ((r3 & 0xF) == 0) { mask |= 4096; n += 1; }
  if ((r3 & 0xF0) == 0) { mask |= 8192; n += 1; }
  if ((r3 & 0xF00) == 0) { mask |= 16384; n += 1; }
  if ((r3 & 0xF000) == 0) { mask |= 32768; n += 1; }

  if (n == 0) return evalBoard(r0, r1, r2, r3);

  const w2: f32 = 0.9 / f32(n);
  const w4: f32 = 0.1 / f32(n);
  let score: f32 = 0;
  let bit: i32 = 1;
  for (let cell = 0; cell < 16; ++cell) {
    if ((mask & bit) != 0) {
      const row = cell >> 2;
      const shift = (cell & 3) * 4;
      let a0 = r0, a1 = r1, a2 = r2, a3 = r3;
      if (row == 0) a0 |= 1 << shift;
      else if (row == 1) a1 |= 1 << shift;
      else if (row == 2) a2 |= 1 << shift;
      else a3 |= 1 << shift;
      score += w2 * maxNode(a0, a1, a2, a3, depth);

      if (row == 0) a0 = r0 | (2 << shift);
      else if (row == 1) a1 = r1 | (2 << shift);
      else if (row == 2) a2 = r2 | (2 << shift);
      else a3 = r3 | (2 << shift);
      score += w4 * maxNode(a0, a1, a2, a3, depth);
    }
    bit <<= 1;
  }

  ttKey[slot] = h;
  ttVal[slot] = score;
  ttDep[slot] = depth;
  return score;
}

// Value of one root direction, restricted to the spawn cell with index `shard`.
// Summing the shards of a direction reproduces the whole root chance node.
export function scoreShard(r0: i32, r1: i32, r2: i32, r3: i32,
                           dir: i32, shard: i32, nShard: i32, depth: i32): f32 {
  const g = applyMove(r0, r1, r2, r3, dir);
  if (g < 0) return -1e30;
  const b0 = t0, b1 = t1, b2 = t2, b3 = t3;
  const w2: f32 = 0.9 / f32(nShard);
  const w4: f32 = 0.1 / f32(nShard);
  let seen = 0;
  let bit: i32 = 1;
  for (let cell = 0; cell < 16; ++cell) {
    const row = cell >> 2;
    const shift = (cell & 3) * 4;
    let empty = false;
    if (row == 0) empty = (b0 & (0xF << shift)) == 0;
    else if (row == 1) empty = (b1 & (0xF << shift)) == 0;
    else if (row == 2) empty = (b2 & (0xF << shift)) == 0;
    else empty = (b3 & (0xF << shift)) == 0;
    if (!empty) continue;
    if (seen == shard) {
      let a0 = b0, a1 = b1, a2 = b2, a3 = b3;
      if (row == 0) a0 |= 1 << shift;
      else if (row == 1) a1 |= 1 << shift;
      else if (row == 2) a2 |= 1 << shift;
      else a3 |= 1 << shift;
      let v = w2 * maxNode(a0, a1, a2, a3, depth);

      a0 = b0; a1 = b1; a2 = b2; a3 = b3;
      if (row == 0) a0 |= 2 << shift;
      else if (row == 1) a1 |= 2 << shift;
      else if (row == 2) a2 |= 2 << shift;
      else a3 |= 2 << shift;
      v += w4 * maxNode(a0, a1, a2, a3, depth);
      return v;
    }
    seen += 1;
    bit <<= 1;
  }
  return -1e30;
}

// Applies a move and returns the score gained (-1 when the move does nothing).
// The resulting rows are read back with outR0..outR3.
export function doMove(r0: i32, r1: i32, r2: i32, r3: i32, dir: i32): f32 {
  return applyMove(r0, r1, r2, r3, dir);
}

export function outR0(): i32 { return t0; }
export function outR1(): i32 { return t1; }
export function outR2(): i32 { return t2; }
export function outR3(): i32 { return t3; }

// A quarter of a shard: one move out of one spawn outcome, unweighted.
//
// The root needs max over moves anyway, so each of those moves can be a task of
// its own. That is what keeps the pool busy: a single shard is up to 0.24 s at
// depth 5, which becomes the whole move's latency once it is the last task.
// `move` 0-3 is the two-tile spawn with that move, 4-7 the four-tile spawn.
export function scoreCellMove(r0: i32, r1: i32, r2: i32, r3: i32,
                              dir: i32, shard: i32, nShard: i32,
                              move: i32, depth: i32): f32 {
  const g = applyMove(r0, r1, r2, r3, dir);
  if (g < 0) return -1e30;
  const b0 = t0, b1 = t1, b2 = t2, b3 = t3;
  const tile: i32 = move < 4 ? 1 : 2;
  const dir2: i32 = move & 3;
  let seen = 0;
  for (let cell = 0; cell < 16; ++cell) {
    const row = cell >> 2;
    const shift = (cell & 3) * 4;
    let empty = false;
    if (row == 0) empty = (b0 & (0xF << shift)) == 0;
    else if (row == 1) empty = (b1 & (0xF << shift)) == 0;
    else if (row == 2) empty = (b2 & (0xF << shift)) == 0;
    else empty = (b3 & (0xF << shift)) == 0;
    if (!empty) continue;
    if (seen == shard) {
      let a0 = b0, a1 = b1, a2 = b2, a3 = b3;
      if (row == 0) a0 |= tile << shift;
      else if (row == 1) a1 |= tile << shift;
      else if (row == 2) a2 |= tile << shift;
      else a3 |= tile << shift;
      if (applyMove(a0, a1, a2, a3, dir2) < 0) return -1e30;
      return expectNode(t0, t1, t2, t3, depth - 1);
    }
    seen += 1;
  }
  return -1e30;
}

// Number of empty cells after a move (0 = illegal).
export function freeAfterMove(r0: i32, r1: i32, r2: i32, r3: i32, dir: i32): i32 {
  const g = applyMove(r0, r1, r2, r3, dir);
  if (g < 0) return 0;
  let n = 0;
  if ((t0 & 0xF) == 0) n += 1;
  if ((t0 & 0xF0) == 0) n += 1;
  if ((t0 & 0xF00) == 0) n += 1;
  if ((t0 & 0xF000) == 0) n += 1;
  if ((t1 & 0xF) == 0) n += 1;
  if ((t1 & 0xF0) == 0) n += 1;
  if ((t1 & 0xF00) == 0) n += 1;
  if ((t1 & 0xF000) == 0) n += 1;
  if ((t2 & 0xF) == 0) n += 1;
  if ((t2 & 0xF0) == 0) n += 1;
  if ((t2 & 0xF00) == 0) n += 1;
  if ((t2 & 0xF000) == 0) n += 1;
  if ((t3 & 0xF) == 0) n += 1;
  if ((t3 & 0xF0) == 0) n += 1;
  if ((t3 & 0xF00) == 0) n += 1;
  if ((t3 & 0xF000) == 0) n += 1;
  return n;
}

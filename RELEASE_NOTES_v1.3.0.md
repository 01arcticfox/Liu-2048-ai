# Liu 2048 AI v1.3.0

This release keeps the proven search defaults and adds the one evaluator change
that passed a paired full-game A/B, plus a much faster browser task scheduler.

## Native builds

- Default evaluator now gives a rank-weighted bonus to merges between large
  tiles (`--himerge 300,9`). A 16-game paired run on seeds 1000-1015 raised the
  average score from 525,374 to 544,612; the 32768 hit count stayed at 7/16, so
  the previous 60% figure from the larger sample still applies.
- `--himerge 0,9` restores the previous evaluation table for A/B.
- All other defaults are unchanged: budget 2^21, 2^18 transposition slots,
  deep-entry replacement, formation pruning on, no positional hard-coding.
- 16-thread speed: about 50 moves/s on the reference machine.

## Browser build

- The root chance node is now cut into **two** WebAssembly tasks per spawn cell
  instead of eight. Each task evaluates all four follow-up moves inside wasm,
  reducing queue traffic from 128 to 32 tasks per move.
- In an eight-worker harness this cut measured 220 ms/move to 66 ms/move with
  identical scores. New-game reset clears the transposition tables of the main
  thread and every worker and restores the starting depth.
- The browser uses the same rank-weighted merge term as the native builds.

## Validation

- Both native builds compile with MSVC; `--selftest` passes.
- Repository copy of the console source rebuilds from scratch.
- Browser engine reset and scheduling verified with a fake-worker harness.
- Source comments are English; no third-party project names remain in the
  release tree.

## Assets

- `2048AI-console.exe` - headless/terminal build.
- `2048AI-web.exe` - WebView2 desktop build.

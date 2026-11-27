// Self-test: runs automatic games with Solver4x4 and reports strength and pace.
// Usage: ai_selftest [games] [maxMovesPerGame]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <thread>
#include "solver/Solver4x4.h"

using Board = std::array<int, 16>;

static std::mt19937 rng(20261121u);

// Directions: 0 up, 1 right, 2 down, 3 left.
static std::vector<int> collapse(std::vector<int> vals, int& gained) {
    std::vector<int> out;
    size_t i = 0;
    while (i < vals.size()) {
        if (i + 1 < vals.size() && vals[i] == vals[i + 1]) {
            const int v = vals[i] * 2;
            out.push_back(v);
            gained += v;
            i += 2;
        } else {
            out.push_back(vals[i]);
            ++i;
        }
    }
    while (out.size() < 4) out.push_back(0);
    return out;
}

static bool applyMove(const Board& in, int dir, Board& out, int& gained) {
    out = in;
    gained = 0;
    bool moved = false;
    const int rStep[4] = { -1, 0, 1, 0 };
    const int cStep[4] = { 0, 1, 0, -1 };

    for (int line = 0; line < 4; ++line) {
        int idx[4];
        for (int i = 0; i < 4; ++i) {
            int r, c;
            switch (dir) {
            case 0: r = i;     c = line;  break;   // 上：从顶边读
            case 1: r = line;  c = 3 - i; break;   // 右：从右边读
            case 2: r = 3 - i; c = line;  break;   // 下
            default: r = line; c = i;     break;   // 左
            }
            idx[i] = r * 4 + c;
        }
        std::vector<int> vals;
        for (int i = 0; i < 4; ++i)
            if (in[idx[i]]) vals.push_back(in[idx[i]]);
        int g = 0;
        const std::vector<int> merged = collapse(vals, g);
        for (int i = 0; i < 4; ++i) {
            if (out[idx[i]] != merged[i]) moved = true;
            out[idx[i]] = merged[i];
        }
        gained += g;
    }
    (void)rStep; (void)cStep;
    return moved;
}

static bool hasMove(const Board& b) {
    if (std::find(b.begin(), b.end(), 0) != b.end()) return true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const int v = b[r * 4 + c];
            if (c < 3 && b[r * 4 + c + 1] == v) return true;
            if (r < 3 && b[(r + 1) * 4 + c] == v) return true;
        }
    return false;
}

static int g_fourRate = 10;   // 生成 4 的概率（%）

static void spawn(Board& b) {
    std::vector<int> empty;
    for (int i = 0; i < 16; ++i) if (!b[i]) empty.push_back(i);
    if (empty.empty()) return;
    std::uniform_int_distribution<size_t> pick(0, empty.size() - 1);
    b[empty[pick(rng)]] = (rng() % 100 < g_fourRate) ? 4 : 2;
}

// Reference path: take the best direction from getAllMoveScores (canonical 0 up, 1 right, 2 down, 3 left).
static int pickDir(Solver4x4& solver, const Board& b, float out[4], int& depthOut) {
    Solver4x4::BoardMatrix m(4, std::vector<int>(4, 0));
    for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = b[i];
    const auto scores = solver.getAllMoveScores(m);
    for (int i = 0; i < 4; ++i) out[i] = -1e30f;
    int bestDir = -1;
    float best = -1e29f;
    for (const auto& s : scores) {
        int cd = 0;
        switch (s.dir) {         // solver: 0 left, 1 right, 2 up, 3 down
        case 0: cd = 3; break;
        case 1: cd = 1; break;
        case 2: cd = 0; break;
        default: cd = 2; break;
        }
        out[cd] = s.score;
        if (s.allowed && s.score > best) { best = s.score; bestDir = cd; }
    }
    depthOut = 0;
    return bestDir;
}

static void dump(const Board& b) {
    for (int r = 0; r < 4; ++r) {
        std::printf("   ");
        for (int c = 0; c < 4; ++c) std::printf("%5d", b[r * 4 + c]);
        std::printf("\n");
    }
}

// Parallel path: one solver instance per root direction, matching the desktop build.
struct ParAi {
    Solver4x4 inst[4];
    void budget(std::size_t b) { for (int i = 0; i < 4; ++i) inst[i].setBudget(b); }
    void clear() { for (int i = 0; i < 4; ++i) inst[i].newGame(); }
    int pick(const Board& b, int depth, float out[4]) {
        Solver4x4::BoardMatrix m(4, std::vector<int>(4, 0));
        for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = b[i];
        if (depth <= 0) depth = inst[0].searchDepth(m);
        float sc[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        std::thread th[4];
        for (int d = 0; d < 4; ++d)
            th[d] = std::thread([this, &m, &sc, d, depth] { sc[d] = inst[d].scoreDirection(m, d, depth); });
        for (int d = 0; d < 4; ++d) th[d].join();
        for (int i = 0; i < 4; ++i) out[i] = -1e30f;
        int bestDir = -1;
        float best = -1e29f;
        for (int d = 0; d < 4; ++d) {
            int cd;
            switch (d) { case 0: cd = 3; break; case 1: cd = 1; break; case 2: cd = 0; break; default: cd = 2; break; }
            out[cd] = sc[d];
            if (sc[d] > -1e29f && sc[d] > best) { best = sc[d]; bestDir = cd; }
        }
        return bestDir;
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "-q") {
        Solver4x4 solver;
        solver.setBudget((std::size_t)1 << 17);
        int v[16];
        while (std::scanf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
            &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]) == 16) {
            Board b{};
            for (int i = 0; i < 16; ++i) b[i] = v[i];
            float sc[4];
            int depth = 0;
            const int dir = pickDir(solver, b, sc, depth);
            std::printf("%d\n", dir);
            std::fflush(stdout);
        }
        return 0;
    }

    int parDepth = 0;
    bool usePar = false;
    if (argc > 1 && std::string(argv[1]) == "-p") {
        usePar = true;
        parDepth = argc > 2 ? std::atoi(argv[2]) : 0;
        for (int i = 1; i < argc - 2; ++i) argv[i] = argv[i + 2];
        argc -= 2;
    }

    const int games = argc > 1 ? std::atoi(argv[1]) : 3;
    const int maxMoves = argc > 2 ? std::atoi(argv[2]) : 2000;
    const int budgetShift = argc > 3 ? std::atoi(argv[3]) : 17;
    const unsigned seed = argc > 4 ? (unsigned)std::atoi(argv[4])
        : (unsigned)std::chrono::steady_clock::now().time_since_epoch().count();
    rng.seed(seed);
    if (argc > 5) g_fourRate = std::atoi(argv[5]);
    std::printf("seed = %u  fourRate = %d%%\n", seed, g_fourRate);

    // ---- Shard check: summing one task per spawn cell must reproduce the whole-direction score ----
    // The desktop build uses this split so every worker can take an independent task.
    // This check verifies that the split neither changes nor drops search value.
    if (argc > 1 && std::string(argv[1]) == "--shard") {
        Solver4x4 probe;
        probe.setBudget((std::size_t)1 << 19);
        Board b{};
        b[0] = 2; b[1] = 4; b[5] = 8; b[6] = 16; b[9] = 32; b[10] = 64; b[13] = 128;
        int compared = 0, mismatched = 0;
        double worstRel = 0;
        for (int step = 0; step < 400; ++step) {
            Solver4x4::BoardMatrix m(4, std::vector<int>(4, 0));
            for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = b[i];
            const int depth = 4;
            for (int d = 0; d < 4; ++d) {
                const int n = probe.freeAfterMove(m, d);
                if (n <= 0) continue;
                const float whole = probe.scoreDirection(m, d, depth);
                if (whole <= -1e29f) continue;
                float sum = 0;
                bool ok = true;
                for (int sh = 0; sh < n; ++sh) {
                    const float v = probe.scoreDirectionShard(m, d, depth, sh, n);
                    if (v <= -1e29f) { ok = false; break; }
                    sum += v;
                }
                if (!ok) continue;
                ++compared;
                const double rel = std::fabs((double)sum - (double)whole) /
                                   std::max(1.0, std::fabs((double)whole));
                if (rel > worstRel) worstRel = rel;
                if (rel > 1e-4) ++mismatched;
            }
            // advance the board with a random legal move
            bool moved = false;
            for (int k = 0; k < 8 && !moved; ++k) {
                const int d = (int)(rng() % 4);
                Board t;
                int g = 0;
                if (applyMove(b, d, t, g)) { b = t; moved = true; }
            }
            if (!moved) break;
            spawn(b);
        }
        std::printf("shard check: %d direction scores compared, %d mismatched, worst rel %.3e\n",
                    compared, mismatched, worstRel);
        return mismatched == 0 ? 0 : 1;
    }

    Solver4x4 solver;
    solver.setBudget((std::size_t)1 << budgetShift);
    static ParAi par;   // 4 个求解器实例约 2MB，放静态区避免爆栈
    par.budget((std::size_t)1 << budgetShift);

    if (usePar) std::printf("mode = parallel, depth = %s\n", parDepth > 0 ? std::to_string(parDepth).c_str() : "auto");
    std::printf("budget = 1<<%d\n", budgetShift);

    // ---- Direction check: only the first column holds two 2-tiles, so only UP merges ----
    {
        Solver4x4::BoardMatrix probe(4, std::vector<int>(4, 0));
        probe[0][0] = 2;
        probe[1][0] = 2;
        const auto boards = solver.debugMoveBoards(probe);
        std::printf("[dir probe] solver index -> resulting top-left / (0,0),(0,3),(3,0):\n");
        for (int d = 0; d < 4; ++d) {
            std::printf("  idx %d :  ", d);
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) std::printf("%d ", boards[d][r][c]);
                std::printf("| ");
            }
            std::printf("\n");
        }
        std::printf("  (expect: the one with a 4 at (0,0) is UP)\n\n");
    }

    // Warm up: keep table initialization out of the first measured sample.
    solver.getAllMoveScores(Solver4x4::BoardMatrix(4, std::vector<int>(4, 0)));

    int overallBest = 0;
    long long totalMs = 0;
    long long totalMoves = 0;

    for (int g = 0; g < games; ++g) {
        Board b{};
        spawn(b);
        spawn(b);

        int moves = 0;
        int maxTile = 2;
        long long gameMs = 0;

        while (moves < maxMoves) {
            Solver4x4::BoardMatrix m(4, std::vector<int>(4, 0));
            for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = b[i];

            const auto t0 = std::chrono::steady_clock::now();
            int bestDir = -1;
            if (usePar) {
                float sc4[4];
                bestDir = par.pick(b, parDepth, sc4);
            } else {
                const auto scores = solver.getAllMoveScores(m);
                float bestScore = -1e29f;
                for (const auto& s : scores) {
                    int cd = 0;
                    switch (s.dir) {         // solver: 0 left, 1 right, 2 up, 3 down
                    case 0: cd = 3; break;
                    case 1: cd = 1; break;
                    case 2: cd = 0; break;
                    default: cd = 2; break;
                    }
                    if (s.allowed && s.score > bestScore) { bestScore = s.score; bestDir = cd; }
                }
            }
            gameMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (bestDir < 0) {
                std::printf("  [stop] no allowed move at move %d\n", moves);
                dump(b);
                break;
            }

            Board nb{};
            int gained = 0;
            if (!applyMove(b, bestDir, nb, gained)) {
                std::printf("  [stop] chosen dir %d made no change\n", bestDir);
                dump(b);
                break;
            }
            b = nb;
            ++moves;
            spawn(b);

            for (int i = 0; i < 16; ++i) if (b[i] > maxTile) maxTile = b[i];

            if (moves % 200 == 0)
                std::printf("    ... move %d  maxTile %d\n", moves, maxTile);
            std::fflush(stdout);
            if (!hasMove(b)) { std::printf("  [stop] board full at move %d\n", moves); dump(b); break; }
        }

        totalMs += gameMs;
        totalMoves += moves;
        if (maxTile > overallBest) overallBest = maxTile;

        std::printf("game %d: moves=%d  maxTile=%d  score-time=%lldms  avg=%.1fms/move\n",
            g + 1, moves, maxTile, gameMs,
            moves ? (double)gameMs / moves : 0.0);
        std::fflush(stdout);
    }

    std::printf("\nbest tile = %d (2^%.2f), moves total=%lld, avg %.1f ms/move\n",
        overallBest, std::log2((double)overallBest) - 1.0 + 1.0,
        totalMoves, totalMoves ? (double)totalMs / totalMoves : 0.0);
    return 0;
}

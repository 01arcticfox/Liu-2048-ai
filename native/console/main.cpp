// ============================================================================
//  liu 2048ai - console edition
//  ---------------------------------------------------------------------------
//  Pure terminal, no third-party dependency. The board is drawn with
//  box-drawing characters and sized so every cell is a real square (derived
//  from the console font metrics). Tiles use a 17-step gradient (empty plus
//  2 .. 65536) and two themes are available: warm and blue.
//  Decisions come from src/solver/Solver4x4.h (Expectimax + bitboard + TT).
//
//  Build (MSVC):
//    cl /std:c++20 /O2 /EHsc /utf-8 /I src console\main.cpp /Fe:2048ai.exe
//  Build (GCC/Clang):
//    g++ -std=c++20 -O2 -I src console/main.cpp -o 2048ai
//
//  Just run the exe - it opens on a ready board and waits for a key:
//    1  let the AI start         2  start playing by hand
//    3  switch Chinese / English  4  switch theme (warm / blue)
//    5  cycle AI speed            arrows / WASD  move by hand
//    SPACE  hand over to AI       R  restart   Q / Esc  quit
//
//  Optional command line (batch mode, no keyboard):
//    2048ai --delay 0                full speed auto play
//    2048ai --target 4096 --endless  stop at 4096, then play until stuck
//    2048ai --games 5 --quiet        5 games, print only the results
//    2048ai --lang en --theme 1      start in English with the blue theme
//    2048ai --selftest               check direction mapping / moves / palette
// ============================================================================

#define NOMINMAX
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <conio.h>
#  include <io.h>
#else
#  include <sys/select.h>
#  include <termios.h>
#  include <unistd.h>
#endif

// Solver4x4.h uses MSVC's __forceinline; provide an equivalent for other compilers
#if !defined(_MSC_VER) && !defined(__forceinline)
#  define __forceinline inline __attribute__((always_inline))
#endif

#include "solver/Solver4x4.h"
#include "ntuple/NTuple.h"

namespace {

constexpr int kN = 4;

// ============================================================================
//  Options / preferences
// ============================================================================

// Search workers: one per root direction, doubled up while there is hardware
// left, but never more than eight (diminishing returns, and plenty of memory
// traffic per worker on the shared table).
inline int defaultThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(hw ? hw : 4), 1, 16);
}

struct Options {
    std::size_t   budget   = 0;     // 0 = scale with the core count
    int           delayMs  = 0;
    int           games    = 1;
    std::uint64_t seed     = 0;
    int           target   = 2048;
    bool          endless  = false;
    bool          color    = true;
    bool          quiet    = false;
    bool          selftest = false;
    bool          manual   = false;
    bool          show     = false;
    int           lang     = 0;   // 0 = Chinese, 1 = English
    int           theme    = 0;   // 0 = warm, 1 = blue
    int           fixedDepth = 0;   // > 0: search exactly this depth
    int           hashBits   = 0;   // 0 = pick from the core count
    int           probeBits  = 0;   // 0 = default depth probe cap
    int           probeEvery = 1;   // 1 = probe every move; >1 reuses the depth while the board keeps filling
    int           maxDepth   = 0;   // 0 = no cap, else hard ceiling on plies
    int           capBoost   = 0;   // added to the free-cell depth caps
    int           dangerDepth = 0;  // > 0: minimum depth at <= 2 free cells
    std::size_t   moveNodes  = 0;       // 0 = probe mode; > 0 = per-move node budget
    int           ttDeep     = 1;   // 1 = accept TT entries searched deeper than asked
    int           perCache   = 0;   // 1 = every worker gets its own table (no sharing)
    int           ttWays     = 1;   // slots per hash bucket (2 measured no gain)
    int           ttSlack    = 0;   // accept TT entries up to N plies shallower
    int           ttPolicy   = 0;   // 0 = overwrite, 1 = keep much deeper foreign slot
    long long     maxMoves   = 0;   // > 0: stop a headless game after N moves
    int           ph2Free    = 0;   // phase-2 weight set: free-cell threshold (0 = off)
    float         ph2W[4]    = {270, 700, 47, 11};
    int           endBoost   = 0;   // extra plies on crowded boards (0 = off)
    int           endFree    = 3;   // ...when free cells <= this
    bool          noshard    = false; // one task per direction (WebView parity)
    // Conversion benchmark: start from a stored board, or store the board at the
    // moment a game first reaches a tile. P(32768) = P(reach 16384) * P(convert),
    // and with the current engine the first factor is ~100%, so the second one is
    // the whole problem - and it can be measured from a fixed position in about a
    // third of the moves a full game needs.
    std::string   startPos;
    std::string   snapPath;
    int           snapAt = 0;
    float         wE = -1, wM = -1, wMo = -1, wS = -1, wSm = -1;  // <0 = keep default
    float         sumExp = -1, monoExp = -1;   // <0 = keep default shape (3.5 / 4.0)
    // Self-play trainer for the row table (see runTrain).
    int           trainGames = 0;
    float         tdAlpha = 0.0005f;
    int           tdZero  = 0;    // 1 = zero-init afterstate TD instead of blurring the heuristic
    // 6-tuple network training (see src/ntuple/NTuple.h).
    int           ntMode   = 0;   // 1 = train, 2 = average several checkpoints
    int           ntDepth  = 1;
    int           ntSpawns = 1;
    float         ntAlpha  = 0.0025f;
    float         ntDecay  = 200000.0f;
    std::string   ntLoad;
    std::string   netPath;      // trained network to score with (empty = heuristic)
    int           ntEvalEvery = 0;
    int           ntEvalGames = 24;
    int           ntSaveEvery = 0;
    float         tdGamma = 0.985f;
    float         tdScale = 1.0f;     // reward divisor: score is already the right unit
    int           tdDepth = 2;        // playout policy depth of the trainer
    std::string   tablePath;          // learned table to load (or to save)
    bool          classic    = false; // BFS depth probe + node budget instead
    int           threads    = defaultThreads();   // search workers
    float         formPen    = -1;  // soft formation anchor penalty (-1 = keep engine default)
    bool          noIntact   = true;  // formation pruning off: 2048 is a chance model
    int           prune4     = 0;     // 0 = both spawns, N = skip 4-spawn at depth <= N
};

// Runtime preferences, changed by the number keys while playing
struct Prefs {
    int         lang    = 0;
    int         theme   = 0;
    int         delayMs = 0;
    int         threads = 8;
};


const int kSpeedPresets[] = {200, 80, 40, 20, 0};   // ms per AI move, 0 = full speed

// ============================================================================
//  Small helpers
// ============================================================================

std::string fmtInt(long long v) {
    if (v < 0) return "-" + fmtInt(-v);
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

std::string fmtScore(float v) {
    char buf[32];
    const double a = std::fabs(static_cast<double>(v));
    if (a >= 1e9)      std::snprintf(buf, sizeof buf, "%.1fe9", v / 1e9);
    else if (a >= 1e6) std::snprintf(buf, sizeof buf, "%.2fe6", v / 1e6);
    else if (a >= 1e3) std::snprintf(buf, sizeof buf, "%.1fe3", v / 1e3);
    else               std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(v));
    return buf;
}

// Display width in terminal columns: CJK characters count as two
std::size_t utf8Width(const std::string& s) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        int len = 1;
        if ((c >> 5) == 0x06)      { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0x0E) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += static_cast<std::size_t>(len);
        const bool wide =
            (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
    }
    return w;
}

// ============================================================================
//  Terminal plumbing
// ============================================================================

struct TermInfo {
    int cellW = 8;    // pixel width of one character cell
    int cellH = 16;   // pixel height of one text line
    int cols  = 80;
    int rows  = 25;
};

TermInfo queryTerminal() {
    TermInfo t;
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX fi{};
    fi.cbSize = sizeof(fi);
    if (GetCurrentConsoleFontEx(h, FALSE, &fi)) {
        if (fi.dwFontSize.X > 0) t.cellW = fi.dwFontSize.X;
        if (fi.dwFontSize.Y > 0) t.cellH = fi.dwFontSize.Y;
    }
    CONSOLE_SCREEN_BUFFER_INFO bi{};
    if (GetConsoleScreenBufferInfo(h, &bi)) {
        t.cols = bi.srWindow.Right - bi.srWindow.Left + 1;
        t.rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    }
#endif
    if (t.cellW <= 0) t.cellW = 8;
    if (t.cellH <= 0) t.cellH = 16;
    if (t.cols <= 0)  t.cols = 80;
    if (t.rows <= 0)  t.rows = 25;
    return t;
}

void initConsole() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"liu 2048ai");
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
                              | ENABLE_PROCESSED_OUTPUT);
    // Disable quick-edit so a stray click cannot freeze the game
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD im = 0;
    if (GetConsoleMode(in, &im))
        SetConsoleMode(in, (im | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
#endif
}

bool stdinIsTTY() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

void sleepMs(int ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

bool keyAvailable() {
#if defined(_WIN32)
    return _kbhit() != 0;
#else
    timeval tv{0, 0};
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(STDIN_FILENO, &fs);
    return select(STDIN_FILENO + 1, &fs, nullptr, nullptr, &tv) > 0;
#endif
}

int readKeyRaw() {
#if defined(_WIN32)
    return _getch();
#else
    return std::getchar();
#endif
}

// ============================================================================
//  Colour: 17-step gradient per theme
// ============================================================================

struct Rgb { int r = 0, g = 0, b = 0; };

struct Paint {
    Rgb bg;
    Rgb fg;
};

struct Theme {
    const char* nameZh;
    const char* nameEn;
    Rgb  border, dim, label, accent, warn, text, empty;
    double h0, h1;    // hue sweep
    double s0, s1;    // saturation sweep
    double l0, l1;    // lightness sweep
};

const Theme kThemes[2] = {
    // warm: cream -> rose -> magenta -> violet
    {"暖色", "warm",
     {74, 74, 92}, {130, 130, 150}, {120, 200, 255}, {237, 194, 46}, {235, 120, 110},
     {230, 230, 240}, {34, 34, 44},
     42, -98, 0.30, 0.85, 0.91, 0.40},
    // blue: pale sky -> azure -> indigo
    {"蓝色", "blue",
     {64, 78, 110}, {122, 140, 172}, {130, 190, 255}, {96, 170, 255}, {240, 120, 120},
     {228, 236, 248}, {26, 32, 46},
     198, 268, 0.32, 0.86, 0.93, 0.38},
};

Rgb hslToRgb(double h, double s, double l) {
    h = std::fmod(h, 360.0);
    if (h < 0) h += 360.0;
    s = std::clamp(s, 0.0, 1.0);
    l = std::clamp(l, 0.0, 1.0);
    const double c = (1 - std::fabs(2 * l - 1)) * s;
    const double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
    const double m = l - c / 2;
    double r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return {static_cast<int>(std::lround((r + m) * 255)),
            static_cast<int>(std::lround((g + m) * 255)),
            static_cast<int>(std::lround((b + m) * 255))};
}

double relLum(Rgb c) {
    auto ch = [](int v) {
        const double x = v / 255.0;
        return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(c.r) + 0.7152 * ch(c.g) + 0.0722 * ch(c.b);
}

// Pick the text colour with the better contrast ratio
Rgb textOn(Rgb bg) {
    const double l = relLum(bg);
    const Rgb dark{119, 110, 101};
    const double withWhite = 1.05 / (l + 0.05);
    const double withDark = (l + 0.05) / (relLum(dark) + 0.05);
    return withWhite >= withDark ? Rgb{249, 246, 242} : dark;
}

struct Palette {
    Paint p[17];   // 0 = empty cell, 1..16 = 2 .. 65536
};

Palette buildPalette(const Theme& th) {
    Palette q{};
    q.p[0].bg = th.empty;
    q.p[0].fg = th.empty;
    for (int i = 1; i <= 16; ++i) {
        const double t = static_cast<double>(i - 1) / 15.0;
        q.p[i].bg = hslToRgb(th.h0 + (th.h1 - th.h0) * t,
                             th.s0 + (th.s1 - th.s0) * t,
                             th.l0 + (th.l1 - th.l0) * t);
        q.p[i].fg = textOn(q.p[i].bg);
    }
    return q;
}

const Palette& paletteAt(int theme) {
    static const Palette cache[2] = {buildPalette(kThemes[0]), buildPalette(kThemes[1])};
    return cache[std::clamp(theme, 0, 1)];
}

int rankOf(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while (v > 1 && r < 16) { v >>= 1; ++r; }
    return r;
}

const Paint& paintOf(int theme, int v) { return paletteAt(theme).p[rankOf(v)]; }

std::string escFg(Rgb c) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
    return buf;
}
std::string escBg(Rgb c) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "\x1b[48;2;%d;%d;%dm", c.r, c.g, c.b);
    return buf;
}
std::string paintText(const std::string& s, Rgb fg, bool color) {
    return color ? escFg(fg) + s + "\x1b[0m" : s;
}

// ============================================================================
//  Localisation
// ============================================================================

struct Strings {
    const char* title;
    const char* tagIdle;
    const char* tagAuto;
    const char* tagManual;
    const char* tagOver;
    const char* tagStopped;
    const char* scoreFmt;    // "%s" receives the formatted number
    const char* bestFmt;
    const char* movesFmt;
    const char* perSec;
    const char* aiTurn;
    const char* aiSuggests;
    const char* pressRestart;
    const char* idleHint;
    const char* keysA;
    const char* keysB;
    const char* langName;
};

const Strings kStrings[2] = {
    {"liu 2048ai·控制台", " 就绪 ", " AI 自动 ", " 手动 ", " 无路可走 ", " 已停手 ",
     "分数 %s", "最高 %s", "第 %s 手", "手/秒",
     "AI 正在走，按方向键随时接管", "AI 建议", "R / 回车 重新开始",
     "按 1 让 AI 开始，或直接按方向键自己走",
     " 1 开始AI  2 手动开局  3 中/EN  4 主题  5 速度",
     " 方向键/WASD 走子   空格 切换 AI   Q 退出",
     "中文"},
    {"liu 2048ai·Console", " Ready ", " AI auto ", " Manual ", " No moves ", " Stopped ",
     "Score %s", "Best %s", "Moves %s", "moves/s",
     "AI is playing - press an arrow to take over", "AI suggests", "R / Enter to restart",
     "Press 1 to start the AI, or just use the arrow keys",
     " 1 start-AI  2 manual  3 中/EN  4 theme  5 speed",
     " Arrows/WASD move   SPACE toggle AI   Q quit",
     "English"},
};

const Strings& str(int lang) { return kStrings[std::clamp(lang, 0, 1)]; }

// ============================================================================
//  Game rules
// ============================================================================

const char* const kArrow[4] = {"↑", "→", "↓", "←"};

// Solver4x4 reports MoveScore.dir as {0 right, 1 left, 2 up, 3 down}, but the
// board it decodes is mirrored left/right (verify with --selftest), so map by the
// board we actually get when applying the move.
constexpr int kSolverToUi[4] = {3, 1, 0, 2};

inline void lineCoord(int dir, int i, int k, int& r, int& c) {
    switch (dir) {
        case 0: r = k;          c = i;          break;   // up
        case 1: r = i;          c = kN - 1 - k; break;   // right
        case 2: r = kN - 1 - k; c = i;          break;   // down
        default: r = i;         c = k;          break;   // left
    }
}

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    std::uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool chance(double p) { return (next() >> 11) * (1.0 / 9007199254740992.0) < p; }
};

struct Board {
    int       c[kN][kN] = {};
    long long score     = 0;
    int       moves     = 0;
    int       maxTile   = 0;

    void reset() {
        std::memset(c, 0, sizeof c);
        score = 0; moves = 0; maxTile = 0;
    }

    void updateMax() {
        for (int r = 0; r < kN; ++r)
            for (int x = 0; x < kN; ++x)
                if (c[r][x] > maxTile) maxTile = c[r][x];
    }

    bool spawn(Rng& rng) {
        int slots[kN * kN];
        int n = 0;
        for (int r = 0; r < kN; ++r)
            for (int x = 0; x < kN; ++x)
                if (c[r][x] == 0) slots[n++] = r * kN + x;
        if (n == 0) return false;
        const int at = slots[rng.below(n)];
        const int v  = rng.chance(0.9) ? 2 : 4;
        c[at / kN][at % kN] = v;
        if (v > maxTile) maxTile = v;
        return true;
    }

    bool applyMove(int dir, int& gained) {
        gained = 0;
        bool moved = false;
        for (int i = 0; i < kN; ++i) {
            int in[kN];
            for (int k = 0; k < kN; ++k) {
                int r, x;
                lineCoord(dir, i, k, r, x);
                in[k] = c[r][x];
            }
            // Compact first, then merge - otherwise "2 0 0 2" never merges
            int comp[kN];
            int cn = 0;
            for (int k = 0; k < kN; ++k)
                if (in[k] != 0) comp[cn++] = in[k];

            int out[kN] = {0, 0, 0, 0};
            int n = 0;
            for (int k = 0; k < cn;) {
                if (k + 1 < cn && comp[k] == comp[k + 1]) {
                    out[n] = comp[k] * 2;
                    gained += out[n];
                    k += 2;
                } else {
                    out[n] = comp[k];
                    ++k;
                }
                ++n;
            }
            for (int k = 0; k < kN; ++k) {
                int r, x;
                lineCoord(dir, i, k, r, x);
                if (c[r][x] != out[k]) moved = true;
                c[r][x] = out[k];
            }
        }
        if (moved) {
            score += gained;
            ++moves;
        }
        return moved;
    }

    bool canMove() const {
        for (int r = 0; r < kN; ++r)
            for (int x = 0; x < kN; ++x) {
                if (c[r][x] == 0) return true;
                if (x + 1 < kN && c[r][x] == c[r][x + 1]) return true;
                if (r + 1 < kN && c[r][x] == c[r + 1][x]) return true;
            }
        return false;
    }
};

// Positions on disk: 16 tile values, row major. Used by the conversion benchmark.
bool loadBoard(const char* path, Board& b) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    b.reset();
    bool ok = true;
    for (int r = 0; r < kN && ok; ++r)
        for (int c = 0; c < kN; ++c)
            if (std::fscanf(f, "%d", &b.c[r][c]) != 1) { ok = false; break; }
    std::fclose(f);
    b.updateMax();
    return ok;
}

void saveBoard(const char* path, const Board& b) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    for (int r = 0; r < kN; ++r) {
        for (int c = 0; c < kN; ++c)
            std::fprintf(f, "%d%c", b.c[r][c], c + 1 == kN ? '\n' : ' ');
    }
    std::fclose(f);
}

// The depth probe materialises millions of distinct boards and was measured at
// 34 ms per move - four times the cost of the search it sizes. The answer only
// drifts slowly (it falls as the board fills), so it is recomputed every few
// moves and allowed to change by at most one or two plies at a time.
struct DepthCache {
    int depth  = 5;
    int atMove = -999;
    int free0  = -1;
};

struct Session {
    DepthCache dc;
    int depthHint = 4;   // where iterative deepening starts on the next move
    Board  b;
    Rng    rng{1};
    bool   over      = false;
    bool   finished  = false;
    bool   started   = false;
    bool   autoPlay  = false;
    bool   won2048   = false;
    int    lastDir   = -1;
    float  lastScore = 0;
    int    hintDir   = -1;
    double elapsed   = 0;

    void restart(std::uint64_t seed) {
        b.reset();
        dc = DepthCache{};
        depthHint = 4;
        rng = Rng(seed);
        over = false;
        finished = false;
        started = false;
        won2048 = false;
        lastDir = -1;
        lastScore = 0;
        hintDir = -1;
        elapsed = 0;
        b.spawn(rng);
        b.spawn(rng);
    }
};

// ============================================================================
//  AI
// ============================================================================

struct Choice {
    int   dir   = -1;
    float score = 0;
};

// Search configuration. moveNodes > 0 enables iterative deepening under a node
// budget, fixedDepth > 0 pins the depth, otherwise the solver picks the depth
// from its BFS probe.
struct Engine {
    int fixedDepth = 0;   // > 0: score exactly this depth, no probing
    int threads    = 1;
    int probeEvery = 1;   // 1 = probe every move (exact); >1 reuses the depth for a few moves
    int maxDepth   = 0;   // 0 = no ceiling on plies
    int capBoost   = 0;   // added to every free-cell depth cap (see chooseMove)
    int dangerDepth = 0;  // > 0: minimum depth once <= 2 cells are free
    std::size_t moveNodes = 0;  // > 0: nodes to spend per move (iterative deepening)
    bool noshard   = false; // score each direction whole instead of per spawn cell
    int  endBoost  = 0;     // extra plies once free cells <= endFree
    int  endFree   = 3;
};

Engine engineOf(const Options& opt) {
    return Engine{opt.fixedDepth, opt.threads, opt.probeEvery, opt.maxDepth,
                  opt.capBoost, opt.dangerDepth, opt.moveNodes, opt.noshard,
                  opt.endBoost, opt.endFree};
}

// Depth costs ~4x per extra ply, so the affordable budget used to scale with the
// number of cores. Measurement killed that idea twice over: the budget decides
// the probe depth, and at 2^23 the search ran 5-8 plies deep and reached 32768 in
// 0 of 24 long games, while 2^19 (depth ~4.4) reached it in 3 of 6 with the same
// evaluator - the deeper search over-commits to short-horizon merges and runs out
// of space at the 16k level. 2^19 is also ~3x faster (292 vs 98 moves/s at 4096),
// and being a constant it makes the play identical on every machine.
std::size_t autoBudget(int) {
    // 2^21 is where the strength is: 40 independent games gave 60% at 32768
    // versus 48% at 2^19 (paired: 10 wins, 5 losses). It costs 3.2x the nodes per
    // move, which is 16 moves/s on one core but 68 moves/s on sixteen.
    return std::size_t(1) << 21;
}

// Same time base as Solver4x4's clock, so a deadline can be handed straight over.
double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double msSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

// Persistent search pool. Creating and joining a thread per depth looked cheap but
// is not: a move runs iterative deepening over four or five depths, and with
// sixteen workers that is sixty-odd thread creations and joins per move, which
// measured out as only 7.1 of the 16 workers ever doing work. The pool keeps the
// threads alive for the whole game and hands them a job per depth instead.
class Pool {
public:
    explicit Pool(int n) {
        for (int i = 1; i < n; ++i) th.emplace_back([this, i] { loop(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Runs f(0) on the caller and f(1..N-1) on the pool, and returns when all of
    // them are done.
    void run(const std::function<void(int)>& f) {
        {
            std::lock_guard<std::mutex> lk(m);
            job = &f;
            finished = 0;
            ++gen;
        }
        cv.notify_all();
        f(0);
        std::unique_lock<std::mutex> lk(m);
        done.wait(lk, [this] { return finished == (int)th.size(); });
        job = nullptr;
    }

private:
    void loop(int i) {
        int seen = 0;
        for (;;) {
            const std::function<void(int)>* f = nullptr;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || gen != seen; });
                if (stop) return;
                seen = gen;
                f = job;
            }
            if (f) (*f)(i);
            {
                std::lock_guard<std::mutex> lk(m);
                if (++finished == (int)th.size()) done.notify_one();
            }
        }
    }

    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cv, done;
    const std::function<void(int)>* job = nullptr;
    int gen = 0, finished = 0;
    bool stop = false;
};

// One pool per process, sized on first use. A game never changes its worker count.
Pool* poolFor(int n) {
    static Pool* p = nullptr;
    static int pn = 0;
    if (!p || pn != n) {
        delete p;
        p = new Pool(n);
        pn = n;
    }
    return p;
}

// Search workers. The main solver owns the BFS buffer and picks the depth; the
// extra instances only carry their own transposition table so that the four root
// moves (and slices of their chance nodes) can run at the same time.
struct Workers {
    Solver4x4*                           main = nullptr;
    std::vector<std::unique_ptr<Solver4x4>> extra;

    int count() const { return 1 + static_cast<int>(extra.size()); }

    // Sum the node counters of every worker so the reported rate is honest.
    Solver4x4::Profile total() const {
        Solver4x4::Profile t = main->profile();
        for (const auto& sv : extra) {
            const Solver4x4::Profile p = sv->profile();
            t.bfsMs += p.bfsMs; t.searchMs += p.searchMs;
            t.bfsNodes += p.bfsNodes; t.searchNodes += p.searchNodes;
            t.moves += p.moves; t.depthSum += p.depthSum; t.cutNodes += p.cutNodes;
        }
        return t;
    }
    void resetProfiles() {
        main->resetProfile();
        for (const auto& sv : extra) sv->resetProfile();
    }
    // Total search nodes over all workers; used as the currency of the per-move
    // node budget, which does not care how the work was split.
    std::size_t nodes() const {
        std::size_t n = main->profile().searchNodes;
        for (const auto& sv : extra) n += sv->profile().searchNodes;
        return n;
    }
};

Choice chooseMove(Workers& w, const Board& b, const Engine& eng, DepthCache& dc) {
    Solver4x4::BoardMatrix mat(kN, std::vector<int>(kN, 0));
    for (int r = 0; r < kN; ++r)
        for (int x = 0; x < kN; ++x) mat[r][x] = b.c[r][x];

    Solver4x4& pool0 = *w.main;
    std::vector<Solver4x4::MoveScore> scored;

    const int workers = std::max(1, w.count());
    // The root chance node of each direction is cut into one task per spawn cell,
    // so the queue holds ~40 short tasks instead of 16 long ones. The four
    // directions leave different numbers of free cells, and a subtree's cost
    // grows with that number, so fixed four-way slices left the biggest task
    // deciding the wall clock and sixteen threads only bought 8%. Summing the
    // slices in a fixed order keeps the chosen move independent of the thread
    // count.
    // --noshard scores each direction in one piece instead, which is what the
    // WebView host does, so the two versions can be compared exactly.
    const bool single = eng.noshard;

    // Score all four directions at a given depth, handing the spawn cells to the
    // workers as they become free.
    float sc[4] = {Solver4x4::INVALID_SCORE, Solver4x4::INVALID_SCORE,
                   Solver4x4::INVALID_SCORE, Solver4x4::INVALID_SCORE};

    int nSpawn[4]  = {0, 0, 0, 0};      // tasks per direction
    int offTask[4] = {0, 0, 0, 0};      // first task index of each direction
    int nTasks     = 0;
    for (int d = 0; d < 4; ++d) {
        const int n = pool0.freeAfterMove(mat, d);
        nSpawn[d] = (n > 0) ? (single ? 1 : n) : 0;
        offTask[d] = nTasks;
        nTasks += nSpawn[d];
    }
    if (nTasks > 60) nTasks = 60;       // 4 directions x 15 spawn cells at most

    auto scoreAtDepth = [&](int depth) {
        if (nTasks == 0) return;
        float part[64] = {0};
        std::atomic<int> next{0};

        auto work = [&](int tid) {
            Solver4x4& sv = (tid == 0) ? pool0 : *w.extra[static_cast<std::size_t>(tid - 1)];
            for (;;) {
                const int id = next.fetch_add(1, std::memory_order_relaxed);
                if (id >= nTasks) break;
                int d = 0;
                while (d < 3 && offTask[d + 1] <= id) ++d;
                const int sh = id - offTask[d];
                part[id] = sv.scoreDirectionShard(mat, d, depth, sh, nSpawn[d]);
            }
        };

        poolFor(workers)->run(work);

        for (int d = 0; d < 4; ++d) sc[d] = Solver4x4::INVALID_SCORE;
        for (int d = 0; d < 4; ++d) {
            if (nSpawn[d] == 0) continue;
            bool ok = true;
            float sum = 0;
            for (int sh = 0; sh < nSpawn[d]; ++sh) {
                const float v = part[offTask[d] + sh];
                if (v <= -1e29f) { ok = false; break; }
                sum += v;
            }
            if (ok) sc[d] = sum;
        }
        scored.clear();
        for (int d = 0; d < 4; ++d) {
            const bool ok = sc[d] > -1e29f;
            scored.push_back({d, sc[d], ok, ok});
        }
    };

    int freeCount = 0;
    for (int r = 0; r < kN; ++r)
        for (int x = 0; x < kN; ++x) freeCount += (b.c[r][x] == 0);

    // Three depth policies:
    //   --depth N        fixed number of plies (controls, no adaptivity)
    //   --nodes N        iterative deepening under a node budget (default)
    //   neither          one BFS probe per move (the reference behaviour)
    // The node budget is what pins the playing speed. The probe counts distinct
    // boards, so on a mid-density board (5-8 free cells) it asks for a depth whose
    // expectimax tree is orders of magnitude larger, and a single move can take
    // seconds; deeper still, and the game never finishes. Deepening until the
    // budget is gone costs the same per move everywhere and spends the whole
    // budget on as many plies as fit at that position.
    if (eng.fixedDepth > 0) {
        scoreAtDepth(std::min(eng.fixedDepth, 14));
    } else if (eng.moveNodes == 0) {
        // The probe is serial and costs 7 ms a move, which is a fifth of the wall
        // clock once sixteen workers are running: parallel work shrinks, the
        // probe does not. Its answer drifts slowly and only downwards as the
        // board fills, so it is reused for a few moves and recomputed as soon as
        // a merge opens the board up again.
        const bool reuse = eng.probeEvery > 0 && dc.atMove >= 0
                           && (b.moves - dc.atMove) < eng.probeEvery
                           && freeCount <= dc.free0;
        int depth;
        if (reuse) {
            depth = dc.depth;
        } else {
            depth = pool0.searchDepth(mat);
            dc.depth  = depth;
            dc.atMove = b.moves;
            dc.free0  = freeCount;
        }
        // Crowded boards are where every game is decided, and there the tree is
        // small: the probe already asks for 6-8 plies, but plain extra plies are
        // cheap there, so buy a couple more exactly where it matters.
        if (eng.endBoost > 0 && freeCount <= eng.endFree)
            depth = std::min(depth + eng.endBoost, 14);
        if (eng.maxDepth > 0) depth = std::min(depth, eng.maxDepth);
        scoreAtDepth(depth);
    } else {
        const std::size_t budget = eng.moveNodes;
        std::size_t costPrev = 0, costPrev2 = 0, spent = 0;
        int reached = 0;
        for (int d = 2; d <= 14; ++d) {
            // Stop before starting a depth whose cost the growth ratio predicts
            // will not fit, but never before half the budget is spent: on a board
            // where the tree grows fast the ratio over-predicts and would quit at
            // depth 4 with three quarters of the budget unused.
            if (reached >= 2 && costPrev > 0 && costPrev2 > 0 && spent * 2 > budget) {
                const double ratio = (double)costPrev / (double)costPrev2;
                const double next  = (double)costPrev * std::min(ratio, 32.0);
                if ((double)spent + next > (double)budget * 1.02) break;
            }
            const std::size_t before = w.nodes();
            scoreAtDepth(d);
            const std::size_t cost = w.nodes() - before;
            spent += cost;
            costPrev2 = costPrev;
            costPrev  = cost;
            reached   = d;
            if (spent >= budget) break;
        }
        dc.depth  = reached;
        dc.atMove = b.moves;
        dc.free0  = freeCount;
        pool0.noteDepth(reached);
    }

    Choice best;
    for (const auto& ms : scored) {
        if (!ms.allowed) continue;
        const int d = kSolverToUi[ms.dir & 3];
        Board probe = b;
        int g = 0;
        if (probe.applyMove(d, g)) {
            if (best.dir < 0 || ms.score > best.score) {
                best.dir = d;
                best.score = ms.score;
            }
        }
    }
    return best;
}

// ============================================================================
//  Rendering
// ============================================================================

struct Layout {
    int cellChars = 8;   // inner width in characters
    int cellLines = 4;   // inner height in text lines
    int cols      = 80;
};

Layout currentLayout(const TermInfo& t) {
    Layout L;
    L.cols = t.cols;

    // Chrome around the board: title, rule, score, info, two key lines = 6.
    // Two blank lines on top and one spare line keep a redraw from scrolling.
    const int budget = t.rows - 6 - 2 - 1;

    const int charsMax = std::clamp((t.cols - 6) / kN, 4, 12);
    const int linesMax = std::clamp((budget - 5) / kN, 2, 8);

    // Build a square cell: height follows the width, then the width is corrected
    // from the height when the window height is the limiting factor.
    const double ratio = static_cast<double>(t.cellW) / static_cast<double>(t.cellH);
    int lines = static_cast<int>(std::lround(charsMax * ratio));
    lines = std::clamp(lines, 2, linesMax);
    int chars = static_cast<int>(std::lround(lines / (ratio > 0.0 ? ratio : 0.5)));
    chars = std::clamp(chars, 4, charsMax);

    L.cellChars = chars;
    L.cellLines = lines;
    return L;
}

// One line when it fits, otherwise break in two (at a comma when possible)
std::vector<std::string> numberLines(int v, int width) {
    const std::string s = fmtInt(v);
    if (static_cast<int>(s.size()) < width) return {s};
    if (static_cast<int>(s.size()) <= width * 2) {
        std::size_t cut = s.rfind(',');
        if (cut == std::string::npos || cut == 0 || cut + 1 >= s.size())
            cut = (s.size() + 1) / 2;
        else
            ++cut;   // keep the comma on the first line
        return {s.substr(0, cut), s.substr(cut)};
    }
    return {s.substr(0, static_cast<std::size_t>(width))};
}

std::string centered(const std::string& s, int width) {
    const int len = static_cast<int>(utf8Width(s));
    if (len >= width) return s;
    const int left = (width - len) / 2;
    return std::string(static_cast<std::size_t>(left), ' ') + s +
           std::string(static_cast<std::size_t>(width - len - left), ' ');
}

void appendTile(std::string& out, int theme, int v, int y, int width, int height, bool color) {
    const Paint& pt = paintOf(theme, v);
    std::string body;
    if (v == 0) {
        body.assign(static_cast<std::size_t>(width), ' ');
    } else {
        const auto lines = numberLines(v, width);
        const int top = (height - static_cast<int>(lines.size())) / 2;
        const int idx = y - top;
        if (idx >= 0 && idx < static_cast<int>(lines.size()))
            body = centered(lines[static_cast<std::size_t>(idx)], width);
        else
            body.assign(static_cast<std::size_t>(width), ' ');
    }
    out += color ? escBg(pt.bg) + escFg(pt.fg) + body + "\x1b[0m" : body;
}

std::string renderFrame(const Session& s, const Prefs& prefs, const Options& opt,
                        const TermInfo& t, bool first) {
    const Layout L = currentLayout(t);
    const Theme& th = kThemes[std::clamp(prefs.theme, 0, 1)];
    const Strings& S = str(prefs.lang);
    const bool color = opt.color;

    std::string out;
    out += first ? "\x1b[2J\x1b[H\x1b[?25l" : "\x1b[H";

    const std::string hbar = "\xe2\x94\x80";   // ─
    auto bar = [&](const char* left, const char* mid, const char* right) {
        std::string r = left;
        for (int i = 0; i < L.cellChars; ++i) r += hbar;
        for (int j = 1; j < kN; ++j) {
            r += mid;
            for (int i = 0; i < L.cellChars; ++i) r += hbar;
        }
        r += right;
        return paintText(r, th.border, color);
    };

    const int boardWidth = 1 + kN * L.cellChars + (kN + 1);

    // Two blank lines on top: keeps the frame off the window edge and gives the
    // redraw some slack so it never scrolls
    out += "\n\n";

    // ---- title with a right aligned mode tag
    const std::string titleLeft = std::string(" ") + S.title;
    std::string tag;
    Rgb tagColor = th.accent;
    if (!s.started)      { tag = S.tagIdle;    tagColor = th.dim; }
    else if (s.over)     { tag = S.tagOver;    tagColor = th.warn; }
    else if (s.finished) { tag = S.tagStopped; tagColor = th.warn; }
    else if (s.autoPlay) { tag = S.tagAuto;    tagColor = th.label; }
    else                 { tag = S.tagManual;  tagColor = th.accent; }

    // theme and speed ride along on the title line so no extra row is needed
    const Theme& cur = kThemes[std::clamp(prefs.theme, 0, 1)];
    char speed[32];
    if (prefs.delayMs > 0) std::snprintf(speed, sizeof speed, "%dms", prefs.delayMs);
    else std::snprintf(speed, sizeof speed, "%s", prefs.lang ? "max" : "\xe5\x85\xa8\xe9\x80\x9f");
    char settings[96];
    char tail[24];
    std::snprintf(tail, sizeof tail, prefs.lang ? "%dt" : "%d\xe6\xa0\xb8", prefs.threads);
    std::snprintf(settings, sizeof settings, "%s\xc2\xb7%s",
                  prefs.lang ? cur.nameEn : cur.nameZh, tail);

    std::string title = paintText(titleLeft, th.label, color);
    const int needed = static_cast<int>(utf8Width(titleLeft)) +
                       static_cast<int>(utf8Width(settings)) +
                       static_cast<int>(utf8Width(tag)) + 1;
    if (needed <= boardWidth) {
        title += paintText(" " + std::string(settings), th.dim, color);
        title += std::string(static_cast<std::size_t>(boardWidth - needed), ' ');
    } else {
        const int pad = boardWidth - static_cast<int>(utf8Width(titleLeft)) -
                        static_cast<int>(utf8Width(tag));
        if (pad > 0) title += std::string(static_cast<std::size_t>(pad), ' ');
    }
    title += paintText(tag, tagColor, color);
    out += title + "\x1b[K\n";

    // ---- rule
    std::string rule;
    for (int i = 0; i < boardWidth - 1; ++i) rule += hbar;
    out += " " + paintText(rule, th.border, color) + "\x1b[K\n";

    // ---- board
    out += " " + bar("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90") + "\x1b[K\n";
    for (int r = 0; r < kN; ++r) {
        for (int y = 0; y < L.cellLines; ++y) {
            out += " " + paintText("\xe2\x94\x82", th.border, color);
            for (int c = 0; c < kN; ++c) {
                appendTile(out, prefs.theme, s.b.c[r][c], y, L.cellChars, L.cellLines, color);
                out += paintText("\xe2\x94\x82", th.border, color);
            }
            out += "\x1b[K\n";
        }
        if (r + 1 < kN)
            out += " " + bar("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4") + "\x1b[K\n";
    }
    out += " " + bar("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98") + "\x1b[K\n";

    // ---- score line, with the current settings right aligned
    char buf[256];
    std::string score = " ";
    std::snprintf(buf, sizeof buf, S.scoreFmt, fmtInt(s.b.score).c_str());
    score += buf;
    score += "   ";
    std::snprintf(buf, sizeof buf, S.bestFmt, fmtInt(s.b.maxTile).c_str());
    score += buf;
    score += "   ";
    std::snprintf(buf, sizeof buf, S.movesFmt, fmtInt(s.b.moves).c_str());
    score += buf;

    out += paintText(score, th.text, color) + "\x1b[K\n";

    // ---- info line
    std::string info = " ";
    if (s.started && s.elapsed > 0.05) {
        std::snprintf(buf, sizeof buf, "%.1fs \xc2\xb7 %.1f %s", s.elapsed,
                      s.b.moves / s.elapsed, S.perSec);
        info += buf;
        info += "   ";
    }
    if (!s.started)      info += S.idleHint;
    else if (s.over)     info += S.pressRestart;
    else if (s.autoPlay) info += S.aiTurn;
    else                 info += std::string(S.aiSuggests) + " ";
    out += paintText(info, th.dim, color);
    if (s.started && !s.over && !s.autoPlay) {
        out += paintText(s.hintDir >= 0 ? kArrow[s.hintDir] : "-", th.accent, color);
        if (s.lastScore != 0)
            out += paintText("  " + fmtScore(s.lastScore), th.dim, color);
    }
    out += "\x1b[K\n";

    // ---- key hints
    out += paintText(S.keysA, th.dim, color) + "\x1b[K\n";
    out += paintText(S.keysB, th.dim, color) + "\x1b[K\n";

    out += "\x1b[J";
    return out;
}

void flushFrame(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

// ============================================================================
//  One game
// ============================================================================

struct GameResult {
    long long score   = 0;
    int       maxTile = 0;
    int       moves   = 0;
    double    seconds = 0;
    bool      reached = false;
};

enum class Key {
    None, Up, Right, Down, Left, Space, Restart, RestartManual,
    Lang, Theme, Speed, Enter, Quit
};

Key decodeKey(int k) {
    switch (k) {
        case 0:
        case 224: {
            const int c = readKeyRaw();
            if (c == 72) return Key::Up;
            if (c == 77) return Key::Right;
            if (c == 80) return Key::Down;
            if (c == 75) return Key::Left;
            return Key::None;
        }
        case 'w': case 'W': return Key::Up;
        case 'd': case 'D': return Key::Right;
        case 's': case 'S': return Key::Down;
        case 'a': case 'A': return Key::Left;
        case ' ':           return Key::Space;
        case '1':           return Key::Restart;
        case '2':           return Key::RestartManual;
        case '3':           return Key::Lang;
        case '4':           return Key::Theme;
        case '5':           return Key::Speed;
        case 'r': case 'R': return Key::Restart;
        case '\r': case '\n': return Key::Enter;
        case 'q': case 'Q': case 27: return Key::Quit;
        default: return Key::None;
    }
}

int dirOfKey(Key k) {
    switch (k) {
        case Key::Up:    return 0;
        case Key::Right: return 1;
        case Key::Down:  return 2;
        case Key::Left:  return 3;
        default:         return -1;
    }
}

std::uint64_t freshSeed() {
    return static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// ---------------------------------------------------------------- interactive
int runInteractive(Workers& workers, const Options& opt) {
    Prefs prefs;
    prefs.lang = opt.lang;
    prefs.theme = opt.theme;
    prefs.delayMs = opt.delayMs;
    prefs.threads = opt.threads;

    const Engine eng = engineOf(opt);
    Session s;
    s.restart(opt.seed ? opt.seed : freshSeed());

    auto start = std::chrono::steady_clock::now();
    auto lastAi = start;
    bool dirty = true;
    bool first = true;
    bool quit = false;

    auto refreshHint = [&] {
        if (s.over) return;
        const Choice c = chooseMove(workers, s.b, eng, s.dc);
        s.hintDir = c.dir;
        s.lastScore = c.score;
    };
    auto doRestart = [&](bool autoMode) {
        // A fresh table per game: entries from the previous run are all for boards
        // that no longer exist and only steal slots from this game.
        workers.main->newGame();
        s.restart(opt.seed ? opt.seed : freshSeed());
        s.started = true;
        s.autoPlay = autoMode;
        start = std::chrono::steady_clock::now();
        lastAi = start;
        if (!s.autoPlay) refreshHint();
        dirty = true;
    };
    if (opt.manual) doRestart(false);   // --manual skips the ready screen

    while (!quit) {
        const auto now = std::chrono::steady_clock::now();
        if (s.started) s.elapsed = std::chrono::duration<double>(now - start).count();

        while (keyAvailable()) {
            const Key k = decodeKey(readKeyRaw());

            if (k == Key::Quit) { quit = true; break; }
            if (k == Key::Restart) { doRestart(true); continue; }
            if (k == Key::RestartManual) { doRestart(false); continue; }
            if (k == Key::Enter) {
                if (s.over) doRestart(s.autoPlay);
                continue;
            }
            if (k == Key::Lang)  { prefs.lang = 1 - prefs.lang; dirty = true; continue; }
            if (k == Key::Theme) { prefs.theme = 1 - prefs.theme; dirty = true; continue; }
            if (k == Key::Speed) {
                int idx = 0;
                for (int i = 0; i < 5; ++i)
                    if (kSpeedPresets[i] == prefs.delayMs) idx = i;
                prefs.delayMs = kSpeedPresets[(idx + 1) % 5];
                dirty = true;
                continue;
            }
            if (k == Key::Space && !s.over) {
                if (!s.started) {
                    s.started = true;
                    s.autoPlay = true;
                    start = std::chrono::steady_clock::now();
                    lastAi = start;
                } else {
                    s.autoPlay = !s.autoPlay;
                    if (!s.autoPlay) refreshHint();
                }
                dirty = true;
                continue;
            }
            const int d = dirOfKey(k);
            if (d >= 0 && !s.over) {
                if (!s.started) {     // first arrow press starts the game by hand
                    s.started = true;
                    start = std::chrono::steady_clock::now();
                }
                s.autoPlay = false;   // pressing a direction takes over immediately
                int gained = 0;
                if (s.b.applyMove(d, gained)) {
                    s.b.spawn(s.rng);
                    s.b.updateMax();
                    s.lastDir = d;
                    if (s.b.maxTile >= 2048) s.won2048 = true;
                    if (!s.b.canMove()) s.over = true;
                    refreshHint();
                }
                dirty = true;
            }
        }
        if (quit) break;

        if (s.started && s.autoPlay && !s.over &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAi).count() >=
                prefs.delayMs) {
            const Choice c = chooseMove(workers, s.b, eng, s.dc);
            if (c.dir < 0) {
                s.over = true;
            } else {
                int gained = 0;
                s.b.applyMove(c.dir, gained);
                s.b.spawn(s.rng);
                s.b.updateMax();
                s.lastDir = c.dir;
                s.lastScore = c.score;
                s.hintDir = c.dir;
                if (s.b.maxTile >= 2048) s.won2048 = true;
                if (!s.b.canMove()) s.over = true;
            }
            lastAi = std::chrono::steady_clock::now();
            dirty = true;
        }

        if (dirty) {
            const TermInfo t = queryTerminal();
            flushFrame(renderFrame(s, prefs, opt, t, first));
            first = false;
            dirty = false;
        }

        sleepMs((s.autoPlay && !s.over) ? 1 : 6);
    }

    std::fputs("\x1b[?25h\x1b[0m\n", stdout);
    std::fflush(stdout);
    return 0;
}

// ============================================================================
//  Self-play trainer for the row table
// ============================================================================
// The evaluator is a sum of eight line values (four rows, four columns), so the
// entire heuristic is a 65536-entry table. Instead of hand-picking five features
// and seven numbers, this trains those 65536 entries by TD(0) on self-play:
//
//   delta = reward + gamma * V(afterstate) - V(state)
//   T[line] += alpha * delta        for the eight lines of the state
//
// The playout policy is a shallow expectimax over the same table, so training
// needs no transposition table and runs at a few thousand moves per second. The
// learned table is dropped in through --table, and the normal search keeps its
// own pruning and depth policy - only the leaf evaluator changes.

inline int rankOfTile(int v) {
    return v <= 0 ? 0 : (int)std::countr_zero((unsigned)v);
}

inline void lineIndices(const Board& b, int* idx) {
    for (int r = 0; r < kN; ++r) {
        int v = 0;
        for (int c = 0; c < kN; ++c) v |= rankOfTile(b.c[r][c]) << (4 * c);
        idx[r] = v;
    }
    for (int c = 0; c < kN; ++c) {
        int v = 0;
        for (int r = 0; r < kN; ++r) v |= rankOfTile(b.c[r][c]) << (4 * r);
        idx[4 + c] = v;
    }
}

inline float tableValue(const Board& b, const float* T) {
    int idx[8];
    lineIndices(b, idx);
    float s = 0;
    for (int i = 0; i < 8; ++i) s += T[idx[i]];
    return s;
}

// Value after one move, averaged over the tile that spawns next.
float spawnAveragedValue(const Board& b, const float* T) {
    int slots[kN * kN], n = 0;
    for (int r = 0; r < kN; ++r)
        for (int c = 0; c < kN; ++c)
            if (b.c[r][c] == 0) slots[n++] = r * kN + c;
    if (n == 0) return tableValue(b, T);
    float sum = 0;
    for (int i = 0; i < n; ++i) {
        Board t = b;
        t.c[slots[i] / kN][slots[i] % kN] = 2;
        sum += 0.9f * tableValue(t, T);
        t.c[slots[i] / kN][slots[i] % kN] = 4;
        sum += 0.1f * tableValue(t, T);
    }
    return sum / (float)n;
}

float expectValue(const Board& b, const float* T, int depth);

float maxValue(const Board& b, const float* T, int depth) {
    float best = -1e30f;
    for (int d = 0; d < 4; ++d) {
        Board t = b;
        int gained = 0;
        if (!t.applyMove(d, gained)) continue;
        const float v = depth <= 1 ? spawnAveragedValue(t, T) : expectValue(t, T, depth - 1);
        if (v > best) best = v;
    }
    return best;
}

float expectValue(const Board& b, const float* T, int depth) {
    if (depth <= 0) return tableValue(b, T);
    int slots[kN * kN], n = 0;
    for (int r = 0; r < kN; ++r)
        for (int c = 0; c < kN; ++c)
            if (b.c[r][c] == 0) slots[n++] = r * kN + c;
    if (n == 0) return tableValue(b, T);
    float sum = 0;
    for (int i = 0; i < n; ++i) {
        Board t = b;
        t.c[slots[i] / kN][slots[i] % kN] = 2;
        sum += 0.9f * maxValue(t, T, depth);
        t.c[slots[i] / kN][slots[i] % kN] = 4;
        sum += 0.1f * maxValue(t, T, depth);
    }
    return sum / (float)n;
}

int runTrain(const Options& opt) {
    // Heap: the solver carries a 256 KB score table, which is too much for the
    // 1 MB default stack this function also uses for the playout recursion.
    auto solver = std::make_unique<Solver4x4>();
    std::vector<float> T(Solver4x4::TABLE_SIZE);
    solver->exportTable(T.data());
    // The analytic table carries a constant baseline in every entry; drop it so
    // the TD deltas do not accumulate a uniform drift.
    for (float& v : T) v -= 200000.0f;

    Rng rng(opt.seed ? opt.seed : 20261124ULL);
    const float alpha  = opt.tdAlpha;
    const float gamma  = opt.tdGamma;
    const int   pdepth = opt.tdDepth;
    const float rscale = 1.0f / opt.tdScale;

    double sumScore = 0, sumMax = 0;
    int    bestMax = 0;
    const int games = opt.trainGames;

    for (int g = 0; g < games; ++g) {
        Board b;
        b.reset();
        b.spawn(rng);
        b.spawn(rng);

        int idx[8];
        for (;;) {
            lineIndices(b, idx);
            const float vBefore = T[idx[0]] + T[idx[1]] + T[idx[2]] + T[idx[3]]
                                + T[idx[4]] + T[idx[5]] + T[idx[6]] + T[idx[7]];

            int bestDir = -1;
            float bestVal = -1e30f;
            for (int d = 0; d < 4; ++d) {
                Board t = b;
                int gained = 0;
                if (!t.applyMove(d, gained)) continue;
                const float v = (pdepth <= 1) ? spawnAveragedValue(t, T.data())
                                              : expectValue(t, T.data(), pdepth - 1);
                if (v > bestVal) { bestVal = v; bestDir = d; }
            }
            if (bestDir < 0) break;

            int gained = 0;
            Board after = b;
            after.applyMove(bestDir, gained);
            const float vAfter = tableValue(after, T.data());

            const float delta = gained * rscale + gamma * vAfter - vBefore;
            const float step  = alpha * delta;
            for (int i = 0; i < 8; ++i) T[idx[i]] += step;

            b = after;
            b.spawn(rng);
            b.updateMax();   // spawn only tracks the spawned tile
            if (b.maxTile > bestMax) bestMax = b.maxTile;
            if (!b.canMove()) break;
        }

        sumScore += (double)b.score;
        sumMax   += (double)b.maxTile;
        if (b.maxTile > bestMax) bestMax = b.maxTile;

        if ((g + 1) % 100 == 0) {
            std::printf("  game %6d  avgScore %8.0f  avgMax %6.0f  bestMax %6d\n",
                        g + 1, sumScore / (g + 1), sumMax / (g + 1), bestMax);
            std::fflush(stdout);
        }
    }

    if (!opt.tablePath.empty()) {
        std::FILE* f = std::fopen(opt.tablePath.c_str(), "wb");
        if (!f) { std::printf("cannot write %s\n", opt.tablePath.c_str()); return 1; }
        std::fwrite(T.data(), sizeof(float), T.size(), f);
        std::fclose(f);
        std::printf("saved %s after %d games (avg score %.0f, avg max %.0f)\n",
                    opt.tablePath.c_str(), games, sumScore / games, sumMax / games);
    }
    return 0;
}

// Zero-initialised afterstate TD(0). The first trainer seeded the table with the
// analytic heuristic, which cannot work: the values it starts from are ~1e5 while
// a move's reward is 1e0-1e3, so the TD error is dominated by the difference
// between two heuristic values and every update just blurred the table. Starting
// at zero makes the reward the only signal, so the table learns a potential that
// predicts future score instead of re-learning what the heuristic already knew.
int runTrainZero(const Options& opt) {
    auto solver = std::make_unique<Solver4x4>();   // for TABLE_SIZE only
    const std::size_t N = Solver4x4::TABLE_SIZE;
    std::vector<float> T(N, 0.0f);
    std::vector<float> visits(N, 0.0f);

    Rng rng(opt.seed ? opt.seed : 20261125ULL);
    const float alpha0 = opt.tdAlpha > 0 ? opt.tdAlpha : 0.001f;
    const float rscale = 1.0f / std::max(1.0f, opt.tdScale);
    const int   games  = opt.trainGames;

    auto bestAfter = [&](const Board& b, float& val, Board& out) {
        int bestDir = -1;
        val = -1e30f;
        for (int d = 0; d < 4; ++d) {
            Board t = b;
            int g = 0;
            if (!t.applyMove(d, g)) continue;
            const float v = tableValue(t, T.data());
            if (v > val) { val = v; bestDir = d; out = t; }
        }
        return bestDir;
    };

    double sumScore = 0, sumMax = 0;
    int bestMax = 0;

    for (int g = 0; g < games; ++g) {
        Board b;
        b.reset();
        b.spawn(rng);
        b.spawn(rng);

        for (;;) {
            float vAfter = 0;
            Board after;
            const int dir = bestAfter(b, vAfter, after);
            if (dir < 0) break;

            int gained = 0;
            Board moved = b;
            moved.applyMove(dir, gained);

            // One sampled spawn, used both as the TD target and as the next state,
            // so the sample is a real transition instead of a hallucinated one.
            Board nxt = moved;
            nxt.spawn(rng);
            nxt.updateMax();

            float vNext = 0;
            Board dummy;
            if (nxt.canMove()) {
                if (bestAfter(nxt, vNext, dummy) < 0) vNext = 0.0f;
            }

            const float target = (float)gained * rscale + vNext;
            const float delta  = target - vAfter;

            int idx[8];
            lineIndices(moved, idx);
            for (int i = 0; i < 8; ++i) {
                const float a = alpha0 / (1.0f + 0.00005f * visits[idx[i]]);
                float& w = T[idx[i]];
                w += a * delta * 0.125f;
                if (w > 1e6f) w = 1e6f;
                else if (w < -1e6f) w = -1e6f;
                visits[idx[i]] += 1.0f;
            }

            b = nxt;
            if (b.maxTile > bestMax) bestMax = b.maxTile;
            if (!b.canMove()) break;
        }

        sumScore += (double)b.score;
        sumMax   += (double)b.maxTile;
        if (b.maxTile > bestMax) bestMax = b.maxTile;

        if ((g + 1) % 200 == 0) {
            std::printf("  game %6d  avgScore %8.0f  avgMax %6.0f  bestMax %6d\n",
                        g + 1, sumScore / (g + 1), sumMax / (g + 1), bestMax);
            std::fflush(stdout);
        }
    }

    if (!opt.tablePath.empty()) {
        std::FILE* f = std::fopen(opt.tablePath.c_str(), "wb");
        if (!f) { std::printf("cannot write %s\n", opt.tablePath.c_str()); return 1; }
        std::fwrite(T.data(), sizeof(float), T.size(), f);
        std::fclose(f);
        std::printf("saved %s after %d games (avg score %.0f, avg max %.0f)\n",
                    opt.tablePath.c_str(), games, sumScore / games, sumMax / games);
    }
    return 0;
}

// ---------------------------------------------------------------- n-tuple
// Ranks of a board, packed one per cell, which is the network's input format.
inline void ranksOf(const Board& b, std::uint8_t* r) {
    for (int i = 0; i < kN * kN; ++i)
        r[i] = (std::uint8_t)rankOfTile(b.c[i / kN][i % kN]);
}

// Value of the position that results from playing `dir`, or -inf when illegal.
inline float afterValue(const nt::Net& net, const Board& b, int dir, Board& out) {
    Board t = b;
    int g = 0;
    if (!t.applyMove(dir, g)) return -1e30f;
    out = t;
    std::uint8_t r[16];
    ranksOf(t, r);
    return net.value(r);
}

// Greedy one-ply policy on afterstate values. The scan starts at a rotating
// offset so that a table of all zeros does not make the game play "up" forever:
// ties are broken by the caller's turn counter, which gives the cold start the
// variety it needs to see different positions.
inline int greedyDir(const nt::Net& net, const Board& b, Board& after, unsigned turn) {
    int bestDir = -1;
    float bestVal = -1e30f;
    const unsigned start = turn & 3u;
    for (unsigned k = 0; k < 4; ++k) {
        const int d = (int)((start + k) & 3u);
        Board t;
        const float v = afterValue(net, b, d, t);
        if (v > bestVal) { bestVal = v; bestDir = d; after = t; }
    }
    return bestDir;
}

// Value of a position one spawn ahead, averaged over every possible spawn. Used
// by the validation player, not by the training loop, where one sampled spawn is
// the cheaper and unbiased TD transition.
float expectAfterSpawn(const nt::Net& net, const Board& b) {
    int slots[16], n = 0;
    for (int r = 0; r < kN; ++r)
        for (int c = 0; c < kN; ++c)
            if (b.c[r][c] == 0) slots[n++] = r * kN + c;
    if (n == 0) return 0.0f;

    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const int r = slots[i] / kN, c = slots[i] % kN;
        for (int t = 0; t < 2; ++t) {
            Board s = b;
            s.c[r][c] = t == 0 ? 2 : 4;
            float best = -1e30f;
            for (int d = 0; d < 4; ++d) {
                Board tmp;
                const float v = afterValue(net, s, d, tmp);
                if (v > best) best = v;
            }
            if (best < -1e29f) best = 0.0f;
            sum += (double)best * (t == 0 ? 0.9 : 0.1);
        }
    }
    return (float)(sum / (double)n);
}

struct NtResult { double avgMax = 0, avgScore = 0; int hit = 0, games = 0, best = 0; };

// Plays `games` games with a depth-2 expectimax on the learned values. This is the
// honest yardstick: the training loop's own greedy policy is much weaker than the
// engine that will eventually use the table.
NtResult ntEval(const nt::Net& net, int games, std::uint64_t seed, int target) {
    Rng rng(seed ? seed : 1);
    NtResult out;
    double sumMax = 0, sumScore = 0;
    for (int g = 0; g < games; ++g) {
        Board b;
        b.reset();
        b.spawn(rng);
        b.spawn(rng);
        for (;;) {
            int bestDir = -1;
            float bestVal = -1e30f;
            for (int d = 0; d < 4; ++d) {
                Board t;
                int gained = 0;
                Board probe = b;
                if (!probe.applyMove(d, gained)) continue;
                const float v = (float)gained * 0.001f + expectAfterSpawn(net, probe);
                if (v > bestVal) { bestVal = v; bestDir = d; }
            }
            if (bestDir < 0) break;
            b.applyMove(bestDir, bestDir);
            b.spawn(rng);
            b.updateMax();
            if (b.maxTile >= target) break;
            if (!b.canMove()) break;
        }
        sumMax += (double)b.maxTile;
        sumScore += (double)b.score;
        if (b.maxTile > out.best) out.best = b.maxTile;
        if (b.maxTile >= target) ++out.hit;
    }
    out.games = games;
    out.avgMax = sumMax / games;
    out.avgScore = sumScore / games;
    return out;
}

int runTrainNT(const Options& opt) {
    auto net = std::make_unique<nt::Net>();
    if (!opt.ntLoad.empty()) {
        if (!net->load(opt.ntLoad.c_str())) {
            std::printf("cannot load %s\n", opt.ntLoad.c_str());
            return 1;
        }
        std::printf("loaded %s\n", opt.ntLoad.c_str());
    }

    Rng rng(opt.seed ? opt.seed : 987654321ULL);
    const int   games     = opt.trainGames;
    const int   depth     = std::max(1, opt.ntDepth);
    const int   spawns    = std::max(1, opt.ntSpawns);
    const float alpha0    = opt.ntAlpha > 0 ? opt.ntAlpha : 0.0025f;
    const float alphaMin  = 0.0001f;
    const float decay     = opt.ntDecay > 0 ? opt.ntDecay : 200000.0f;
    const float rscale    = 0.001f;   // keeps values around the O(100) the TD step is tuned for
    const int   evalEvery = opt.ntEvalEvery;

    double winMax = 0, winScore = 0;
    int    winN = 0, bestMax = 0, hits = 0;

    const auto t0 = std::chrono::steady_clock::now();

    for (int g = 0; g < games; ++g) {
        const float alpha = std::max(alphaMin, alpha0 / (1.0f + (float)g / decay));

        Board b;
        b.reset();
        b.spawn(rng);
        b.spawn(rng);

        unsigned turn = 0;
        for (;;) {
            Board after;
            const int dir = greedyDir(*net, b, after, turn++);
            if (dir < 0) break;

            int gained = 0;
            Board moved;
            {
                Board probe = b;
                probe.applyMove(dir, gained);
                moved = probe;
            }

            // TD target: one spawn ahead, then the best follow-up. Sampling a
            // single spawn keeps the inner loop cheap and the estimate unbiased;
            // --ntspawns trades speed for a quieter target.
            float vNext = 0;
            for (int s = 0; s < spawns; ++s) {
                Board nxt = moved;
                nxt.spawn(rng);
                float best = 0;
                if (nxt.canMove()) {
                    Board dummy;
                    for (int d = 0; d < 4; ++d) {
                        Board t;
                        const float v = afterValue(*net, nxt, d, t);
                        if (v > best) best = v;
                    }
                }
                vNext += best;
            }
            vNext /= (float)spawns;

            std::uint8_t r[16];
            ranksOf(moved, r);
            const float vNow   = net->value(r);
            const float target = (float)gained * rscale + vNext;
            net->update(r, target - vNow, alpha);

            b = moved;
            b.spawn(rng);
            b.updateMax();
            if (b.maxTile > bestMax) bestMax = b.maxTile;
            if (!b.canMove()) break;
        }

        winMax += (double)b.maxTile;
        winScore += (double)b.score;
        ++winN;
        if (b.maxTile >= 32768) ++hits;

        if ((g + 1) % 1000 == 0) {
            const double secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            std::printf("  game %7d  avgMax %6.0f  avgScore %8.0f  bestMax %6d  a %.5f  %.0f g/s\n",
                        g + 1, winMax / winN, winScore / winN, bestMax, alpha,
                        (double)(g + 1) / secs);
            std::fflush(stdout);
            winMax = winScore = 0;
            winN = 0;
        }

        if (evalEvery > 0 && (g + 1) % evalEvery == 0) {
            const NtResult r = ntEval(*net, std::max(8, opt.ntEvalGames), (std::uint64_t)rng.next(), 32768);
            std::printf("  [eval %d games at depth 2] avgMax %.0f  avgScore %.0f  best %d  hit32768 %d/%d\n",
                        r.games, r.avgMax, r.avgScore, r.best, r.hit, r.games);
            std::fflush(stdout);
        }
        if (opt.ntSaveEvery > 0 && (g + 1) % opt.ntSaveEvery == 0 && !opt.tablePath.empty()) {
            if (net->save(opt.tablePath.c_str()))
                std::printf("  saved %s at game %d\n", opt.tablePath.c_str(), g + 1);
            std::fflush(stdout);
        }
    }

    if (!opt.tablePath.empty() && net->save(opt.tablePath.c_str()))
        std::printf("saved %s after %d games\n", opt.tablePath.c_str(), games);
    std::printf("training games reaching 32768: %d / %d\n", hits, games);
    return 0;
}

// Averages several independently trained networks into one file.
int runNtAverage(const Options& opt) {
    nt::Net acc;
    std::size_t n = 0;
    std::string list = opt.ntLoad;
    std::size_t pos = 0;
    while (pos <= list.size()) {
        const std::size_t comma = list.find(',', pos);
        const std::string path = list.substr(pos, comma == std::string::npos ? std::string::npos
                                                                            : comma - pos);
        if (!path.empty()) {
            nt::Net one;
            if (one.load(path.c_str())) {
                const float* w = one.data();
                float* a = acc.data();
                if (n == 0) std::memcpy(a, w, nt::WEIGHTS * sizeof(float));
                else for (std::size_t i = 0; i < nt::WEIGHTS; ++i) a[i] += w[i];
                ++n;
            } else {
                std::printf("skip %s\n", path.c_str());
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (n == 0) { std::printf("nothing to average\n"); return 1; }
    if (n > 1) {
        float* a = acc.data();
        const float inv = 1.0f / (float)n;
        for (std::size_t i = 0; i < nt::WEIGHTS; ++i) a[i] *= inv;
    }
    if (!acc.save(opt.tablePath.c_str())) {
        std::printf("cannot write %s\n", opt.tablePath.c_str());
        return 1;
    }
    std::printf("averaged %zu networks into %s\n", n, opt.tablePath.c_str());
    const NtResult r = ntEval(acc, std::max(8, opt.ntEvalGames), 12345, 32768);
    std::printf("[eval %d games] avgMax %.0f  avgScore %.0f  best %d  hit32768 %d/%d\n",
                r.games, r.avgMax, r.avgScore, r.best, r.hit, r.games);
    return 0;
}

// ---------------------------------------------------------------- headless
GameResult playHeadless(Workers& workers, const Options& opt, std::uint64_t seed, int index) {
    Prefs prefs;
    prefs.lang = opt.lang;
    prefs.theme = opt.theme;
    prefs.delayMs = opt.delayMs;
    prefs.threads = opt.threads;

    const Engine eng = engineOf(opt);
    Session s;
    s.restart(seed);
    s.started = true;
    s.autoPlay = true;
    // Conversion benchmark: replay from a stored position instead of a fresh board.
    if (!opt.startPos.empty()) {
        if (!loadBoard(opt.startPos.c_str(), s.b)) {
            std::printf("cannot read position %s\n", opt.startPos.c_str());
            return GameResult{};
        }
        s.dc = DepthCache{};
    }
    workers.main->newGame();

    const auto start = std::chrono::steady_clock::now();
    // G2048_DUMP keeps the last few boards of the game, so the position that
    // killed the engine can be inspected after a headless run.
    const bool dumpHist = std::getenv("G2048_DUMP") != nullptr;
    bool snapTaken = false;
    std::vector<Board> hist;
    bool first = true;
    if (opt.show) std::fputs("\x1b[2J\x1b[?25l", stdout);

    while (!s.over) {
        if (opt.maxMoves > 0 && (long long)s.b.moves >= opt.maxMoves) break;
        if (std::getenv("G2048_DEBUG") && s.b.moves % 10 == 0)
            std::fprintf(stderr, "  [dbg] move %d max %d\n", s.b.moves, s.b.maxTile);
        const Choice c = chooseMove(workers, s.b, eng, s.dc);
        if (c.dir < 0) break;
        int gained = 0;
        s.b.applyMove(c.dir, gained);
        s.b.spawn(s.rng);
        s.b.updateMax();
        s.lastDir = c.dir;
        s.lastScore = c.score;
        s.hintDir = c.dir;
        s.elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (s.b.maxTile >= 2048) s.won2048 = true;
        if (opt.snapAt > 0 && !opt.snapPath.empty() && !snapTaken
            && s.b.maxTile >= opt.snapAt) {
            saveBoard(opt.snapPath.c_str(), s.b);
            snapTaken = true;
        }
        if (dumpHist) {
            hist.push_back(s.b);
            if (hist.size() > 8) hist.erase(hist.begin());
            if (s.b.moves % 500 == 0) {
                int empt = 0;
                for (int r = 0; r < kN; ++r)
                    for (int x = 0; x < kN; ++x) empt += (s.b.c[r][x] == 0);
                std::printf("  [trace] move %5d  max %5d  empty %2d  corners %5d %5d %5d %5d\n",
                            s.b.moves, s.b.maxTile, empt,
                            s.b.c[0][0], s.b.c[0][kN - 1], s.b.c[kN - 1][0], s.b.c[kN - 1][kN - 1]);
            }
        }

        if (!opt.endless && s.b.maxTile >= opt.target) break;
        if (!s.b.canMove()) { s.over = true; break; }

        if (opt.show) {
            flushFrame(renderFrame(s, prefs, opt, queryTerminal(), first));
            first = false;
        }
        if (opt.delayMs > 0) sleepMs(opt.delayMs);
    }

    const auto end = std::chrono::steady_clock::now();
    if (opt.show) {
        s.finished = true;
        s.over = !s.b.canMove();
        s.elapsed = std::chrono::duration<double>(end - start).count();
        flushFrame(renderFrame(s, prefs, opt, queryTerminal(), false));
        std::fputs("\x1b[?25h\n", stdout);
    }

    if (std::getenv("G2048_STATS")) {
        const auto p = workers.total();
        const double tot = p.bfsMs + p.searchMs;
        std::printf("  [prof] moves %llu  avgDepth %.2f  bfs %.0fms  search %.1fM nodes %.0fms  cut %.1fM (%.0f%%)\n",
                    p.moves, p.moves ? (double)p.depthSum / (double)p.moves : 0.0,
                    p.bfsMs, (double)p.searchNodes / 1e6, p.searchMs,
                    (double)p.cutNodes / 1e6,
                    p.searchNodes ? 100.0 * (double)p.cutNodes / (double)p.searchNodes : 0.0);
    }
    workers.resetProfiles();

    if (dumpHist && !hist.empty()) {
        std::printf("  [dump] final %d moves, max %d, last %d boards:\n",
                    s.b.moves, s.b.maxTile, (int)hist.size());
        for (const Board& hb : hist) {
            std::printf("    +---------------------------+\n");
            for (int r = 0; r < kN; ++r) {
                std::printf("    |");
                for (int x = 0; x < kN; ++x) {
                    const int tile = hb.c[r][x];   // stored as the tile value, 0 = empty
                    if (tile == 0) std::printf("      .");
                    else           std::printf(" %6d", tile);
                }
                std::printf(" |\n");
            }
        }
        std::printf("    +---------------------------+\n");
    }

    GameResult r;
    r.score = s.b.score;
    r.maxTile = s.b.maxTile;
    r.moves = s.b.moves;
    r.seconds = std::chrono::duration<double>(end - start).count();
    r.reached = s.b.maxTile >= opt.target;

    if (opt.games > 1)
        std::printf(" game %d  best %-6d  score %-10lld  moves %-5d  %.1fs%s\n",
                    index + 1, r.maxTile, r.score, r.moves, r.seconds,
                    r.reached ? "  ok" : "");
    return r;
}

// ============================================================================
//  Self test
// ============================================================================

void printPlain(const Board& b) {
    for (int r = 0; r < kN; ++r) {
        std::printf("    ");
        for (int x = 0; x < kN; ++x) std::printf("%6d", b.c[r][x]);
        std::printf("\n");
    }
}

void runSelfTest(Solver4x4& solver) {
    Board b;
    const int init[kN][kN] = {
        {2, 0, 4, 0},
        {0, 0, 0, 0},
        {0, 8, 0, 0},
        {2, 0, 0, 16},
    };
    for (int r = 0; r < kN; ++r)
        for (int x = 0; x < kN; ++x) b.c[r][x] = init[r][x];

    std::printf("== start board ==\n");
    printPlain(b);

    std::printf("\n== hand written move logic ==\n");
    for (int d = 0; d < 4; ++d) {
        Board t = b;
        int g = 0;
        const bool mv = t.applyMove(d, g);
        std::printf("  dir %d (%s) moved=%d gained=%d\n", d, kArrow[d], (int)mv, g);
        printPlain(t);
    }

    std::printf("\n== solver view ==\n");
    Solver4x4::BoardMatrix mat(kN, std::vector<int>(kN, 0));
    for (int r = 0; r < kN; ++r)
        for (int x = 0; x < kN; ++x) mat[r][x] = b.c[r][x];

    std::printf("  depth = %d\n", solver.debugDepth(mat));
    for (const auto& ms : solver.getAllMoveScores(mat))
        std::printf("  solver dir %d  score %.4g  allowed %d\n", ms.dir, ms.score, (int)ms.allowed);
    std::printf("  decoded boards (order right,left,up,down)\n");
    const char* names[4] = {"right", "left", "up", "down"};
    const auto boards = solver.debugMoveBoards(mat);
    for (int i = 0; i < 4; ++i) {
        Board t;
        for (int r = 0; r < kN; ++r)
            for (int x = 0; x < kN; ++x) t.c[r][x] = boards[i][r][x];
        std::printf("  -- %s --\n", names[i]);
        printPlain(t);
    }

    std::printf("\n== number wrapping (cell width 10 / 8) ==\n");
    const int samples[] = {65536, 131072, 1048576, 16777216};
    for (int v : samples) {
        for (int w : {10, 8}) {
            const auto ls = numberLines(v, w);
            std::printf("  %9d  w%2d -> ", v, w);
            for (std::size_t i = 0; i < ls.size(); ++i)
                std::printf("%s\"%s\"", i ? " | " : "", ls[i].c_str());
            std::printf("\n");
        }
    }

    for (int th = 0; th < 2; ++th) {
        std::printf("\n== 17 step palette: %s ==\n", kThemes[th].nameEn);
        for (int i = 0; i <= 16; ++i) {
            const int v = (i == 0) ? 0 : (1 << i);
            const Paint& p = paintOf(th, v);
            std::printf("  rank %2d  %6d   bg #%02x%02x%02x  fg #%02x%02x%02x\n", i, v,
                        p.bg.r, p.bg.g, p.bg.b, p.fg.r, p.fg.g, p.fg.b);
        }
    }
}

// ============================================================================
//  Command line
// ============================================================================

void printUsage() {
    std::printf(
        "liu 2048ai - console edition\n"
        "\n"
        "Run it and use the keys: 1 restart(AI)  2 manual  3 中/EN  4 theme  5 speed\n"
        "                         arrows/WASD move  SPACE toggle AI  R restart  Q quit\n"
        "\n"


        "  --budget N      depth-probe budget (default 2^21 = depth ~5.2, 60% at\n"
        "                  32768 over 40 games; 2^19 is 3x faster but only 48%)\n"
        "  --delay MS      AI move interval, 0 = full speed (default 0)\n"
        "  --games N       run N games headless (batch mode)\n"
        "  --seed N        fixed random seed\n"
        "  --target N      stop after reaching this tile (default 2048)\n"
        "  --endless       keep playing until no move is left\n"
        "  --manual        start in manual mode\n"
        "  --lang 0|1      0 Chinese, 1 English\n"
        "  --theme 0|1     0 warm, 1 blue\n"
        "  --no-color      disable colours\n"
        "  --quiet         no board, print the result only\n"
        "  --show          draw the board even when output is redirected\n"
        "  --threads N     search workers, 1-16 (default: every logical core)\n"
        "  --prune4 [N]    skip the 4-spawn branch at depth <= N (default 0 = never)\n"
        "                  non-zero makes play non-reproducible and is not recommended)\n"
        "  --intact        turn formation pruning back on (default off: 2048 is a\n"
        "                  chance model, so dropping legal directions deletes part\n"
        "                  of the expectation; measured 46%% vs 60%% with it on)\n"
        "  --formpen N     soft formation anchor penalty (default 0 = off)\n"
        "  --hash N        transposition table slots = 2^N (default 18 = 4 MB,\n"
        "                  which is what fits in L2)\n"
        "  --probe N       depth probe may materialise at most 2^N boards\n"
        "  --maxdepth N    hard ceiling on search plies (0 = none)\n"
        "  --endboost P,F  add P plies once free cells <= F (default off)\n"
        "  --nodes N       node budget per move, iterative deepening (0 = off);\n"
        "                  this is what fixes the playing speed, e.g. 8.7M = 55 moves/s\n"
        "  --ttshallow     only accept exact-depth transposition entries (A/B switch;\n"
        "                  the default accepts entries searched at least as deep)\n"
        "  --ttpolicy N    0 = overwrite the slot, 1 = keep a much deeper foreign\n"
        "                  entry (measured worse: 11%% more nodes, fewer hits)\n"
        "  --proberefresh N  recompute the depth probe every N moves (default 1 =\n"
        "                  every move; >1 reuses it while the board keeps filling)\n"
        "  --maxmoves N    stop a headless game after N moves (benchmarks)\n"
        "  --capboost N    add N to every free-cell depth cap (deeper endgame)\n"
        "  --w E,M,MO,S,SM heuristic weights (empty,merges,mono,sum,smooth)\n"
        "  --shape P,Q     magnitude-curve exponents, pow(rank,P) for sum and\n"
        "                  pow(rank,Q) for monotonicity (default 3.5,4.0)\n"
        "                  (default 0,6 = off)\n"
        "  --train N       self-play train the 65536-entry row table for N games\n"
        "  --tdalpha A     TD step size (default 0.0005)\n"
        "  --tdgamma G     TD discount (default 0.985)\n"
        "  --tddepth D     playout policy depth while training (default 2)\n"
        "  --table FILE    load a learned row table (or where --train writes it)\n"
        "  --depth N       search exactly N plies (0 = solver default)\n"
        "  --think MS      iterative deepening within MS per move\n"
        "  --selftest      check direction mapping / moves / palette\n"
        "  -h, --help      this help\n");
}

bool parseU64(const char* s, std::uint64_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s) return false;
    out = v;
    return true;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](std::uint64_t& v) {
            if (i + 1 < argc) return parseU64(argv[++i], v);
            return false;
        };
        if (a == "-h" || a == "--help") { printUsage(); return false; }
        else if (a == "--budget")   { std::uint64_t v; if (value(v)) { opt.budget = (std::size_t)v; opt.classic = true; } }
        else if (a == "--classic")  opt.classic = true;
        else if (a == "--hash")     { std::uint64_t v; if (value(v)) opt.hashBits = (int)v; }
        else if (a == "--delay")    { std::uint64_t v; if (value(v)) opt.delayMs = (int)v; }
        else if (a == "--games")    { std::uint64_t v; if (value(v)) opt.games = (int)v; }
        else if (a == "--seed")     { std::uint64_t v; if (value(v)) opt.seed = v; }
        else if (a == "--target")   { std::uint64_t v; if (value(v)) opt.target = (int)v; }
        else if (a == "--lang")     { std::uint64_t v; if (value(v)) opt.lang = (int)(v & 1); }
        else if (a == "--theme")    { std::uint64_t v; if (value(v)) opt.theme = (int)(v & 1); }
        else if (a == "--endless")  opt.endless = true;
        else if (a == "--manual")   opt.manual = true;
        else if (a == "--no-color") opt.color = false;
        else if (a == "--quiet")    opt.quiet = true;
        else if (a == "--show")     opt.show = true;
        else if (a == "--depth")    { std::uint64_t v; if (value(v)) opt.fixedDepth = (int)v; }
        else if (a == "--nointact") opt.noIntact = true;
        else if (a == "--intact")   opt.noIntact = false;
        else if (a == "--formpen") { std::uint64_t v; if (value(v)) opt.formPen = (float)v; }
        else if (a == "--noshard")  opt.noshard = true;
        else if (a == "--startpos") { if (i + 1 < argc) opt.startPos = argv[++i]; }
        else if (a == "--snapat")   { std::uint64_t v; if (value(v)) opt.snapAt = (int)v; }
        else if (a == "--snapfile") { if (i + 1 < argc) opt.snapPath = argv[++i]; }
        else if (a == "--prune4") {
            std::uint64_t v = 1;
            if (i + 1 < argc) { std::uint64_t t = 0; if (parseU64(argv[i + 1], t)) { v = t; ++i; } }
            opt.prune4 = (int)v;
        }
        else if (a == "--threads")  { std::uint64_t v; if (value(v)) opt.threads = (int)v; }
        else if (a == "--probe")    { std::uint64_t v; if (value(v)) opt.probeBits = (int)v; }
        else if (a == "--proberefresh") { std::uint64_t v; if (value(v)) opt.probeEvery = (int)v; }
        else if (a == "--maxdepth") { std::uint64_t v; if (value(v)) opt.maxDepth = (int)v; }
        else if (a == "--nodes")    { std::uint64_t v; if (value(v)) opt.moveNodes = (std::size_t)v; }
        else if (a == "--ttdeep")   opt.ttDeep = 1;
        else if (a == "--ttshallow") opt.ttDeep = 0;
        else if (a == "--percache") opt.perCache = 1;
        else if (a == "--ttways")   { std::uint64_t v; if (value(v)) opt.ttWays = (int)v; }
        else if (a == "--ttslack")  { std::uint64_t v; if (value(v)) opt.ttSlack = (int)v; }
        else if (a == "--ttpolicy") { std::uint64_t v; if (value(v)) opt.ttPolicy = (int)v; }
        else if (a == "--maxmoves") { std::uint64_t v; if (value(v)) opt.maxMoves = (long long)v; }
        else if (a == "--w2") {                  // "freeCells,E,M,MO,S"
            float vals[5] = {0, -1, -1, -1, -1};
            const char* c = argv[++i];
            for (int k = 0; k < 5 && c && *c; ++k) {
                char* end = nullptr;
                vals[k] = std::strtof(c, &end);
                if (end == c) break;
                c = (*end == ',') ? end + 1 : nullptr;
            }
            opt.ph2Free = (int)vals[0];
            for (int k = 0; k < 4; ++k) if (vals[k + 1] >= 0) opt.ph2W[k] = vals[k + 1];
        }
        else if (a == "--endboost") {            // "plies,freeThreshold" e.g. "2,3"
            const char* c = argv[++i];
            for (int k = 0; k < 2 && c && *c; ++k) {
                char* end = nullptr;
                const long v = std::strtol(c, &end, 10);
                if (end == c) break;
                if (k == 0) opt.endBoost = (int)v; else opt.endFree = (int)v;
                c = (*end == ',') ? end + 1 : nullptr;
            }
        }
        else if (a == "--capboost") { std::uint64_t v; if (value(v)) opt.capBoost = (int)v; }
        else if (a == "--danger") { std::uint64_t v; if (value(v)) opt.dangerDepth = (int)v; }
        else if (a == "--w") {
            // five comma separated weights: empty,merges,mono,sum,smooth
            float out[5] = {-1, -1, -1, -1, -1};
            const char* c = argv[++i];
            for (int k = 0; k < 5 && c && *c; ++k) {
                char* end = nullptr;
                out[k] = std::strtof(c, &end);
                if (end == c) break;
                c = (*end == ',') ? end + 1 : nullptr;
            }
            opt.wE = out[0]; opt.wM = out[1]; opt.wMo = out[2];
            opt.wS = out[3]; opt.wSm = out[4];
        }
        else if (a == "--shape") {
            // two comma separated exponents: sum, mono (default 3.5,4.0)
            float out[2] = {-1, -1};
            const char* c = argv[++i];
            for (int k = 0; k < 2 && c && *c; ++k) {
                char* end = nullptr;
                out[k] = std::strtof(c, &end);
                if (end == c) break;
                c = (*end == ',') ? end + 1 : nullptr;
            }
            opt.sumExp = out[0]; opt.monoExp = out[1];
        }
        else if (a == "--train")    { std::uint64_t v; if (value(v)) opt.trainGames = (int)v; }
        else if (a == "--tdalpha")  { opt.tdAlpha = std::strtof(argv[++i], nullptr); }
        else if (a == "--tdzero")   opt.tdZero = 1;
        else if (a == "--ntrain")   { std::uint64_t v; if (value(v)) { opt.trainGames = (int)v; opt.ntMode = 1; } }
        else if (a == "--ntdepth")  { std::uint64_t v; if (value(v)) opt.ntDepth = (int)v; }
        else if (a == "--ntspawns") { std::uint64_t v; if (value(v)) opt.ntSpawns = (int)v; }
        else if (a == "--ntalpha")  { opt.ntAlpha = std::strtof(argv[++i], nullptr); }
        else if (a == "--ntdecay")  { opt.ntDecay = std::strtof(argv[++i], nullptr); }
        else if (a == "--ntload")   { opt.ntLoad = argv[++i]; }
        else if (a == "--nteval")   { std::uint64_t v; if (value(v)) opt.ntEvalGames = (int)v; }
        else if (a == "--ntevalevery") { std::uint64_t v; if (value(v)) opt.ntEvalEvery = (int)v; }
        else if (a == "--ntsaveevery") { std::uint64_t v; if (value(v)) opt.ntSaveEvery = (int)v; }
        else if (a == "--ntavg")    { opt.ntMode = 2; opt.ntLoad = argv[++i]; }
        else if (a == "--net")      { opt.netPath = argv[++i]; }
        else if (a == "--tdgamma")  { opt.tdGamma = std::strtof(argv[++i], nullptr); }
        else if (a == "--tddepth")  { std::uint64_t v; if (value(v)) opt.tdDepth = (int)v; }
        else if (a == "--tdscale")  { opt.tdScale = std::strtof(argv[++i], nullptr); }
        else if (a == "--table")    { if (i + 1 < argc) opt.tablePath = argv[++i]; }

        else if (a == "--selftest") opt.selftest = true;
        else {
            std::printf("unknown option: %s\n\n", a.c_str());
            printUsage();
            return false;
        }
    }
    if (opt.games < 1) opt.games = 1;
    if (opt.budget != 0 && opt.budget < 1024) opt.budget = 1024;   // 0 = auto
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 0;

    initConsole();

    const int nThreads = std::clamp(opt.threads, 1, 16);
    // One line so the effective configuration is verifiable on any machine: the
    // budget fixes the strength, the thread count is whatever the box has.
    if (!opt.quiet) {
        const std::size_t eff = opt.budget ? opt.budget : autoBudget(nThreads);
        std::fprintf(stderr, "[cfg] budget %zu (2^%.2f)  threads %d  table 2^%d  ttdeep %d\n",
                     eff, std::log2((double)eff), nThreads,
                     opt.hashBits > 0 ? opt.hashBits : 18, opt.ttDeep != 0);
    }
    if (std::getenv("G2048_DEBUG"))
        std::fprintf(stderr, "  [dbg] opt.budget %zu auto %zu nThreads %d think %d\n",
                     opt.budget, autoBudget(nThreads), nThreads, opt.probeEvery);

    // Two 256 KB line tables live inside, so this cannot sit on the stack: the
    // default 1 MB stack overflows before the first move.
    static Solver4x4 solver;
    solver.setBudget(opt.budget ? opt.budget : autoBudget(nThreads));
    solver.setIgnoreFormation(opt.noIntact);
    if (opt.formPen >= 0) solver.setFormPenalty(opt.formPen);
    solver.setTtPolicy(opt.ttPolicy);
    solver.setPrune4(opt.prune4);
    solver.setTtDeep(opt.ttDeep != 0);
    solver.setTtWays(opt.ttWays);
    solver.setTtSlack(opt.ttSlack);
    if (opt.ph2Free > 0)
        solver.setPhase2(opt.ph2Free, opt.ph2W[0], opt.ph2W[1], opt.ph2W[2], opt.ph2W[3]);
    if (opt.sumExp > 0 || opt.monoExp > 0) {
        float cur[2]; solver.shape(cur);
        solver.setShape(opt.sumExp > 0 ? opt.sumExp : cur[0],
                        opt.monoExp > 0 ? opt.monoExp : cur[1]);
    }
    if (opt.wE >= 0) {
        float cur[5]; solver.weights(cur);
        solver.setWeights(opt.wE, opt.wM >= 0 ? opt.wM : cur[1],
                          opt.wMo >= 0 ? opt.wMo : cur[2],
                          opt.wS  >= 0 ? opt.wS  : cur[3],
                          opt.wSm >= 0 ? opt.wSm : cur[4]);
    }
    // Table size follows the machine: a bigger table means fewer re-searches, and
    // measurements say the win keeps coming until the table stops fitting in cache
    // hierarchy. 8 bytes per slot, so 2^23 is 64 MB, 2^24 is 128 MB.
    if (opt.probeBits > 0) solver.setProbeCap((std::size_t)1 << opt.probeBits);
    // One fixed size for every machine: the table is part of the search state, so
    // a different size means a different set of rare key collisions and therefore
    // a slightly different game. 2^22 slots = 64 MB, enough for the 2^19 search
    // budget and shared by every worker.
    // 2^18 (4 MB) is the size the engine settled on: it fits in L2. Larger tables
    // were measured on the 32768 position - 2^20 cut nodes by 10% with one worker
    // but was 2% slower with sixteen, because the table then competes with the
    // workers' own state for the same cache. One size, one behaviour.
    const int defaultBits = 18;
    solver.setCacheBits(opt.hashBits > 0 ? opt.hashBits : defaultBits);

    if (opt.ntMode == 1) return runTrainNT(opt);
    if (opt.ntMode == 2) return runNtAverage(opt);
    if (opt.trainGames > 0) return opt.tdZero ? runTrainZero(opt) : runTrain(opt);

    // A learned row table replaces the analytic heuristic (see runTrain).
    std::vector<float> learned;
    if (!opt.tablePath.empty()) {
        learned.resize(Solver4x4::TABLE_SIZE);
        std::FILE* f = std::fopen(opt.tablePath.c_str(), "rb");
        if (!f) {
            std::printf("cannot read table %s\n", opt.tablePath.c_str());
            return 1;
        }
        const std::size_t got = std::fread(learned.data(), sizeof(float), learned.size(), f);
        std::fclose(f);
        if (got != learned.size()) {
            std::printf("table %s: %zu floats, expected %d\n",
                        opt.tablePath.c_str(), got, Solver4x4::TABLE_SIZE);
            return 1;
        }
        solver.importTable(learned.data());
    }

    if (opt.selftest) {
        runSelfTest(solver);
        return 0;
    }

    Workers workers;
    workers.main = &solver;
    // A trained 6-cell network scores every instance, so the root directions stay
    // comparable.
    std::unique_ptr<nt::Net> net;
    if (!opt.netPath.empty()) {
        net = std::make_unique<nt::Net>();
        if (!net->load(opt.netPath.c_str())) {
            std::printf("cannot load network %s\n", opt.netPath.c_str());
            return 1;
        }
        solver.setNet(net.get());
        std::printf("network %s attached\n", opt.netPath.c_str());
    }
    for (int i = 1; i < nThreads; ++i) {
        auto sv = std::make_unique<Solver4x4>();
        // Share the table first: setBudget() would otherwise allocate a private
        // one only to throw it away. Helper instances never run the BFS depth
        // probe, so they only need a tiny buffer.
        if (!opt.perCache) sv->shareCacheWith(solver);
        else              sv->setCacheBits(opt.hashBits > 0 ? opt.hashBits : defaultBits);
        sv->setBudget(1024);
        sv->setIgnoreFormation(opt.noIntact);
        if (opt.formPen >= 0) sv->setFormPenalty(opt.formPen);
        sv->setTtPolicy(opt.ttPolicy);
        sv->setPrune4(opt.prune4);
        sv->setTtDeep(opt.ttDeep != 0);
        sv->setTtWays(opt.ttWays);
        sv->setTtSlack(opt.ttSlack);
        if (opt.ph2Free > 0)
            sv->setPhase2(opt.ph2Free, opt.ph2W[0], opt.ph2W[1], opt.ph2W[2], opt.ph2W[3]);
        if (!learned.empty()) sv->importTable(learned.data());
        {
            // Every helper must score with exactly the same evaluator as the main
            // instance, otherwise the four root directions are not comparable.
            float sh[2]; solver.shape(sh);
            if (opt.sumExp > 0 || opt.monoExp > 0) sv->setShape(sh[0], sh[1]);
        }
        if (opt.wE >= 0) {
            float cur[5]; solver.weights(cur);
            sv->setWeights(cur[0], cur[1], cur[2], cur[3], cur[4]);
        }
        workers.extra.push_back(std::move(sv));
    }

    const bool interactive = stdinIsTTY() && !opt.quiet && opt.games == 1;
    if (interactive) return runInteractive(workers, opt);

    std::vector<GameResult> all;
    for (int g = 0; g < opt.games; ++g) {
        const std::uint64_t seed = opt.seed ? opt.seed + static_cast<std::uint64_t>(g)
                                            : freshSeed() + static_cast<std::uint64_t>(g);
        all.push_back(playHeadless(workers, opt, seed, g));
    }

    if (opt.games > 1) {
        long long totalScore = 0;
        double totalTime = 0;
        double totalMax = 0;
        int best = 0;
        int reached = 0;
        for (const auto& r : all) {
            totalScore += r.score;
            totalTime += r.seconds;
            totalMax += r.maxTile;
            best = std::max(best, r.maxTile);
            reached += r.reached ? 1 : 0;
        }
        const double n = static_cast<double>(all.size());
        std::printf("\n %d games  avg score %.0f  avg best %d  record %d  reached %d/%d  %.1fs\n",
                    static_cast<int>(all.size()), totalScore / n,
                    static_cast<int>(std::lround(totalMax / n)), best, reached,
                    static_cast<int>(all.size()), totalTime);
    } else if (opt.quiet) {
        const auto& r = all.front();
        std::printf(" best %d  score %lld  moves %d  %.1fs\n", r.maxTile, r.score,
                    r.moves, r.seconds);
    }

    return 0;
}

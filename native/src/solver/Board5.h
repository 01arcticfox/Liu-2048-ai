#pragma once
// Five-bit cell packing, which is what makes 65536 representable.
//
// The shipping engine packs one 4-bit rank per cell, so rank 15 (32768) is the
// last tile: two 32768s merge back into one 32768 and the chain stops there.
// This header keeps the same ideas - a move table per line, a bit-matrix
// transpose - but with five bits per cell, so rank 16 (65536) exists.
//
// Sixteen cells at five bits is eighty bits, which does not fit a uint64. The
// board is four 32-bit words, one per row, each holding four five-bit fields in
// its low twenty bits. Horizontal moves are one lookup per row; vertical moves
// go through the transpose, exactly as in the 4-bit engine.
//
// Nothing here decides anything on its own. It is a data structure plus a
// self-test (bench/board5_test.cpp) that proves it agrees cell for cell with the
// 4-bit engine on every position the 4-bit engine can represent.

#include <cstdint>
#include <cstring>

namespace b5 {

inline constexpr int LINE_ENTRIES = 1 << 20;   // four cells at five bits

struct Board {
    uint32_t r[4];

    bool operator==(const Board& o) const { return std::memcmp(r, o.r, sizeof r) == 0; }
    bool operator!=(const Board& o) const { return !(*this == o); }
};

__forceinline uint32_t cellGet(uint32_t row, int i) { return (row >> (5 * i)) & 31u; }

__forceinline uint32_t cellSet(uint32_t row, int i, uint32_t v) {
    const int s = 5 * i;
    return (row & ~(31u << s)) | ((v & 31u) << s);
}

// Reverses the four fields of a row, so the right-move table is the left-move
// table applied to a reversed row. Keeps one 4 MB table instead of two.
__forceinline uint32_t reverseRow(uint32_t row) {
    return cellGet(row, 0) << 15 | cellGet(row, 1) << 10 |
           cellGet(row, 2) << 5  | cellGet(row, 3);
}

// The engine's slide rule, verbatim from Solver4x4::moveLine, with the clamp
// lifted from 15 to 31. Each tile merges at most once, and a merge takes the
// pair that is furthest along the direction of travel.
inline uint32_t slideLine(uint32_t row) {
    uint8_t line[4] = {
        (uint8_t)cellGet(row, 0), (uint8_t)cellGet(row, 1),
        (uint8_t)cellGet(row, 2), (uint8_t)cellGet(row, 3),
    };
    int furthest = 4;
    bool merged = false;
    for (int i = 3; i >= 0; --i) {
        if (line[i] == 0) continue;
        if (!merged && furthest < 4 && line[i] == line[furthest]) {
            line[furthest] = (uint8_t)(line[furthest] + 1);   // no saturation: five bits
            line[i] = 0;
            merged = true;
        } else if (furthest == i + 1) {
            furthest = i;
        } else {
            furthest -= 1;
            line[furthest] = line[i];
            line[i] = 0;
            merged = false;
        }
    }
    return (uint32_t)line[0] | ((uint32_t)line[1] << 5) |
           ((uint32_t)line[2] << 10) | ((uint32_t)line[3] << 15);
}

// Slide and merge along +index, and along -index. sLeft is slideLine itself;
// sRight is its mirror image.
__forceinline uint32_t sLeft(uint32_t row)  { return slideLine(row); }
__forceinline uint32_t sRight(uint32_t row) { return reverseRow(slideLine(reverseRow(row))); }

// Move table, built once. One entry per 20-bit row, holding the slid row.
struct Tables {
    uint32_t left[LINE_ENTRIES];

    Tables() {
        for (uint32_t i = 0; i < (uint32_t)LINE_ENTRIES; ++i) left[i] = slideLine(i);
    }
};

inline const Tables& tables() {
    static const Tables t;
    return t;
}

__forceinline uint32_t tLeft(uint32_t row)  { return tables().left[row]; }
__forceinline uint32_t tRight(uint32_t row) { return reverseRow(tables().left[reverseRow(row)]); }

// Four rows by four columns transpose over five-bit fields. Sixteen extracts and
// sixteen inserts; the compiler unrolls this into shifts and masks.
inline Board transpose(const Board& b) {
    Board o;
    for (int c = 0; c < 4; ++c) {
        uint32_t col = 0;
        for (int r = 0; r < 4; ++r) col |= cellGet(b.r[r], c) << (5 * r);
        o.r[c] = col;
    }
    return o;
}

struct Moves { Board up, right, down, left; };

inline Moves moves(const Board& b) {
    Moves m{};
    const Board t = transpose(b);
    for (int i = 0; i < 4; ++i) {
        m.left.r[i]  = tLeft(b.r[i]);
        m.right.r[i] = tRight(b.r[i]);
        m.up.r[i]    = tLeft(t.r[i]);
        m.down.r[i]  = tRight(t.r[i]);
    }
    m.up   = transpose(m.up);
    m.down = transpose(m.down);
    return m;
}

// --- helpers used by the search --------------------------------------------

inline Board fromRanks(const uint8_t rank[16]) {
    Board b;
    for (int i = 0; i < 4; ++i) {
        b.r[i] = (uint32_t)rank[4 * i] | ((uint32_t)rank[4 * i + 1] << 5) |
                 ((uint32_t)rank[4 * i + 2] << 10) | ((uint32_t)rank[4 * i + 3] << 15);
    }
    return b;
}

inline void toRanks(const Board& b, uint8_t rank[16]) {
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) rank[4 * i + c] = (uint8_t)cellGet(b.r[i], c);
}

inline int emptyCount(const Board& b) {
    int n = 0;
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c)
            if (cellGet(b.r[i], c) == 0) ++n;
    return n;
}

inline int maxRank(const Board& b) {
    int m = 0;
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) {
            const int v = (int)cellGet(b.r[i], c);
            if (v > m) m = v;
        }
    return m;
}

// Highest rank anywhere on the board, as a bit mask over cells. Used by the
// equivalence test to skip positions the 4-bit engine cannot hold.
inline uint32_t highCellMask(const Board& b, int threshold) {
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c)
            if ((int)cellGet(b.r[i], c) >= threshold) mask |= 1u << (4 * i + c);
    return mask;
}

// 80-bit hash input. The low word carries the four rows folded; the tag adds
// what folding loses. Two independent multiplies keep the table spread even.
__forceinline uint64_t hashOf(const Board& b) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)b.r[0] * 0x100000001B3ULL;
    h *= 0xD6E8FEB86659FD93ULL;
    h ^= (uint64_t)b.r[1] * 0xC2B2AE3D27D4EB4FULL;
    h *= 0xD6E8FEB86659FD93ULL;
    h ^= (uint64_t)b.r[2] * 0x165667B19E3779F9ULL;
    h *= 0xD6E8FEB86659FD93ULL;
    h ^= (uint64_t)b.r[3] * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 31;
    return h ? h : 1;
}

}  // namespace b5

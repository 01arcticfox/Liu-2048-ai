#pragma once
// 6-cell n-tuple value function for 2048.
//
// The linear evaluation in Solver4x4 scores a board as the sum of eight 4-cell
// line tables. That basis can only see four cells at a time, so it cannot express
// "this 2x3 block is a dead end", which is exactly the judgement the endgame
// needs. This network uses eight overlapping 2x3 rectangles instead: still a sum
// of per-pattern tables, but each pattern sees six cells and the patterns cover
// the board in both orientations.
//
// Each pattern is indexed by the six tile ranks packed four bits apart, so one
// table holds 16^6 = 16.7M floats (67 MB) and the whole network 537 MB.
//
// V(board) = sum over patterns of T[p][index_p(board)]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace nt {

inline constexpr int PATTERNS = 8;
inline constexpr int CELLS    = 6;
inline constexpr int TABLE    = 1 << 24;          // 16^6
inline constexpr std::size_t WEIGHTS =
    (std::size_t)PATTERNS * (std::size_t)TABLE;   // 134M floats, 537 MB

// Eight 2x3 windows: four horizontal, four vertical, covering the board
// symmetrically so a transposed position scores the same.
inline constexpr int PAT[PATTERNS][CELLS] = {
    { 0, 1, 2, 4, 5, 6},      // rows 0-1, cols 0-2
    { 1, 2, 3, 5, 6, 7},      // rows 0-1, cols 1-3
    { 8, 9,10,12,13,14},      // rows 2-3, cols 0-2
    { 9,10,11,13,14,15},      // rows 2-3, cols 1-3
    { 0, 1, 4, 5, 8, 9},      // cols 0-1, rows 0-2
    { 4, 5, 8, 9,12,13},      // cols 0-1, rows 1-3
    { 2, 3, 6, 7,10,11},      // cols 2-3, rows 0-2
    { 6, 7,10,11,14,15},      // cols 2-3, rows 1-3
};

class Net {
public:
    Net() : m_w(new float[WEIGHTS]()) {}

    float*       data()       { return m_w.get(); }
    const float* data() const { return m_w.get(); }

    // rank[] holds one 4-bit tile exponent per cell, 0 for an empty cell.
    inline float value(const std::uint8_t* rank) const {
        const float* base = m_w.get();
        float s = 0.0f;
        for (int p = 0; p < PATTERNS; ++p) {
            std::uint32_t idx = 0;
            const int* at = PAT[p];
            for (int i = 0; i < CELLS; ++i)
                idx |= (std::uint32_t)rank[at[i]] << (4 * i);
            s += base[idx + ((std::size_t)p << 24)];
        }
        return s;
    }

    // Same value straight from the solver's bitboard: the search packs one 4-bit
    // rank per cell, which is already the network's input format, so no unpacking
    // into an array is needed in the hot path.
    inline float valueFromBits(std::uint64_t b) const {
        const float* base = m_w.get();
        float s = 0.0f;
        for (int p = 0; p < PATTERNS; ++p) {
            const int* at = PAT[p];
            std::uint32_t idx = 0;
            for (int i = 0; i < CELLS; ++i)
                idx |= (std::uint32_t)((b >> (4 * at[i])) & 0xFULL) << (4 * i);
            s += base[idx + ((std::size_t)p << 24)];
        }
        return s;
    }

    // Gradient of value() w.r.t. every weight in the pattern is 1, so a TD step
    // is the same additive update on all eight. Dividing by PATTERNS keeps the
    // step size independent of how many patterns the network happens to use.
    inline void update(const std::uint8_t* rank, float delta, float alpha) {
        const float step = alpha * delta * (1.0f / (float)PATTERNS);
        float* base = m_w.get();
        for (int p = 0; p < PATTERNS; ++p) {
            std::uint32_t idx = 0;
            const int* at = PAT[p];
            for (int i = 0; i < CELLS; ++i)
                idx |= (std::uint32_t)rank[at[i]] << (4 * i);
            base[idx + ((std::size_t)p << 24)] += step;
        }
    }

    bool save(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return false;
        const std::uint32_t hdr[3] = {(std::uint32_t)PATTERNS, (std::uint32_t)CELLS,
                                      (std::uint32_t)TABLE};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(m_w.get(), sizeof(float), WEIGHTS, f);
        const bool ok = std::fclose(f) == 0;
        return ok;
    }

    bool load(const char* path) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::uint32_t hdr[3] = {0, 0, 0};
        if (std::fread(hdr, sizeof hdr, 1, f) != 1) { std::fclose(f); return false; }
        if (hdr[0] != (std::uint32_t)PATTERNS || hdr[1] != (std::uint32_t)CELLS ||
            hdr[2] != (std::uint32_t)TABLE) {
            std::fclose(f);
            return false;   // a table from another network shape would be garbage
        }
        const std::size_t got = std::fread(m_w.get(), sizeof(float), WEIGHTS, f);
        std::fclose(f);
        return got == WEIGHTS;
    }

    // Weighted average with another network, used to merge the eight independent
    // trainers: they estimate the same fixed point, so averaging reduces the
    // variance that independent trajectories introduce.
    void averageWith(const float* other) {
        float* w = m_w.get();
        for (std::size_t i = 0; i < WEIGHTS; ++i) w[i] = 0.5f * (w[i] + other[i]);
    }

private:
    std::unique_ptr<float[]> m_w;
};

}   // namespace nt

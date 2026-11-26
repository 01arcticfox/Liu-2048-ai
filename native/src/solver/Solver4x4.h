#ifndef SOLVER_4X4_H
#define SOLVER_4X4_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <mutex>
#include <algorithm>
#include "../ntuple/NTuple.h"
#include <bit>
#include <chrono>
#include <atomic>
#include <memory>
#if defined(_MSC_VER)
#  include <emmintrin.h>   // 128-bit aligned loads/stores for the TT slot
#endif

// ============ C++17 compatibility macros ============
#if __cplusplus < 202002L
// C++17 replacement for std::bit_cast
template <typename To, typename From>
inline To bit_cast(const From& src) noexcept {
	static_assert(sizeof(To) == sizeof(From), "size mismatch");
	To dst;
	std::memcpy(&dst, &src, sizeof(To));
	return dst;
}

// C++17 replacement for std::countr_zero
inline int countr_zero(unsigned int x) {
	if (x == 0) return 32;
#if defined(_MSC_VER)
	unsigned long idx;
	_BitScanForward(&idx, x);
	return (int)idx;
#else
	return __builtin_ctz(x);
#endif
}

// C++17 replacement for std::popcount
inline int popcount(uint64_t x) {
#if defined(_MSC_VER)
	return (int)__popcnt64(x);
#else
	return __builtin_popcountll(x);
#endif
}

// Map std:: versions to our implementations via macros
#define std_bit_cast bit_cast
#define std_countr_zero countr_zero
#define std_popcount popcount
#else
#define std_bit_cast std::bit_cast
#define std_countr_zero std::countr_zero
#define std_popcount std::popcount
#endif
// ========================================

class Solver4x4 {
public:
	using BoardMatrix = std::vector<std::vector<int>>;

	struct MoveScore {
		int   dir;
		float score;
		bool  safe;
		bool  allowed;
	};

	static constexpr float INVALID_SCORE = -1e30f;

	// Profiling counters. Cheap enough to leave in; compare bfs vs search cost.
	struct Profile {
		double bfsMs = 0, searchMs = 0;
		unsigned long long bfsNodes = 0, searchNodes = 0;
		int lastDepth = 0;
		unsigned long long moves = 0;
		unsigned long long depthSum = 0;
		unsigned long long cutNodes = 0;
	};
	mutable Profile m_prof;

	static double nowMs() {
		using namespace std::chrono;
		return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	}

	Solver4x4() {
		initMoveTable();
		initHeuristic();
		setBudget(1u << 17);
	}
	~Solver4x4() = default;

	Solver4x4(const Solver4x4&) = delete;
	Solver4x4& operator=(const Solver4x4&) = delete;

	void setBudget(std::size_t budget) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (budget < 2) budget = 2;
		m_budget = budget;
		// The depth probe never needs the whole search budget: it only has to tell
		// 4 plies from 5 from 6, and materialising millions of distinct boards to
		// do that costs far more than the search it is sizing. Cap its buffer.
		m_bfsBuf.assign(std::min(budget, m_probeCap), 0);
		if (m_radixCount.size() < 65536) m_radixCount.assign(65536, 0);
		if (m_cache->bits == 0) allocCacheLocked(CACHE_BITS);
		cacheClear();
	}

	void newGame()      { std::lock_guard<std::mutex> lk(m_mutex); cacheClear(); }

	void resetProfile() { m_prof = Profile{}; }
	Profile profile() const { return m_prof; }
	// Record a depth the host picked by itself (node-budget mode), so the reported
	// average depth stays comparable with the probe mode.
	void noteDepth(int d) { ++m_prof.moves; if (d > 0) m_prof.depthSum += (unsigned)d; }
	void clearCache()   { std::lock_guard<std::mutex> lk(m_mutex); cacheClear(); }

	// ---- Row-table access, used by the self-play trainer.
	// The evaluator is a pure sum of eight row/column values, so the whole
	// heuristic can be replaced by 65536 numbers learned from play.
	static constexpr int TABLE_SIZE = 65536;
	void exportTable(float* dst) const {
		std::memcpy(dst, m_scoreTable, sizeof(float) * TABLE_SIZE);
	}
	void importTable(const float* src) {
		std::lock_guard<std::mutex> lk(m_mutex);
		std::memcpy(m_scoreTable, src, sizeof(float) * TABLE_SIZE);
	}
	// Value of a board under the current table (no search, no mutex: the trainer
	// runs single threaded against its own copy).
	float tableValue(const BoardMatrix& board) const { return evaluate(encode(board)); }

	std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);

		std::vector<MoveScore> out;
		out.reserve(4);

		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);

		float scores[4] = { INVALID_SCORE, INVALID_SCORE, INVALID_SCORE, INVALID_SCORE };
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;

		const int officialOrder[4] = { 2, 0, 3, 1 };
		for (int oi = 0; oi < 4; ++oi) {
			const int d = officialOrder[oi];
			if (arr[d] != data && (m_ignoreFormation || intact(f, arr[d]))) validDirs[nValid++] = d;
		}
		if (nValid == 0) {
			for (int oi = 0; oi < 4; ++oi) {
				const int d = officialOrder[oi];
				if (arr[d] != data) validDirs[nValid++] = d;
			}
		}

		if (nValid == 0) {
			for (int d = 0; d < 4; ++d)
				out.push_back({ d, INVALID_SCORE, false, false });
			return out;
		}

		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validDirs[i]];

		const double t0 = nowMs();
		const int depth = bfsExpand(validBoards, nValid) + 1;
		const double t1 = nowMs();

		for (int i = 0; i < nValid; ++i) {
			const int d = validDirs[i];
			scores[d] = scoreFrontier(validBoards[i], (uint8_t)depth);
		}
		m_prof.bfsMs += t1 - t0;
		m_prof.searchMs += nowMs() - t1;
		m_prof.lastDepth = depth;
		m_prof.depthSum += (unsigned)depth;
		++m_prof.moves;

		for (int d = 0; d < 4; ++d) {
			const bool ok = scores[d] > -1e29f;
			out.push_back({ d, scores[d], ok, ok });
		}

		std::sort(out.begin(), out.end(),
			[](const MoveScore& a, const MoveScore& b) { return a.score > b.score; });
		return out;
	}

	// ---- Fixed-depth scoring, used by the host for iterative deepening.
	// Shares the transposition table and never clears it. The depth argument means
	// exactly what it means in getAllMoveScores (1 = one move plus one spawn).
	std::vector<MoveScore> scoreAllAtDepth(const BoardMatrix& board, int depth) {
		std::lock_guard<std::mutex> lk(m_mutex);

		std::vector<MoveScore> out;
		out.reserve(4);

		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);

		float scores[4] = { INVALID_SCORE, INVALID_SCORE, INVALID_SCORE, INVALID_SCORE };
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;

		const int officialOrder[4] = { 2, 0, 3, 1 };
		for (int oi = 0; oi < 4; ++oi) {
			const int d = officialOrder[oi];
			if (arr[d] != data && (m_ignoreFormation || intact(f, arr[d]))) validDirs[nValid++] = d;
		}
		if (nValid == 0) {
			for (int oi = 0; oi < 4; ++oi) {
				const int d = officialOrder[oi];
				if (arr[d] != data) validDirs[nValid++] = d;
			}
		}

		if (nValid == 0) {
			for (int d = 0; d < 4; ++d)
				out.push_back({ d, INVALID_SCORE, false, false });
			return out;
		}

		const uint8_t d8 = (uint8_t)std::clamp(depth, 1, 63);
		for (int i = 0; i < nValid; ++i) {
			const int d = validDirs[i];
			scores[d] = scoreFrontier(arr[d], d8);
		}

		for (int d = 0; d < 4; ++d) {
			const bool ok = scores[d] > -1e29f;
			out.push_back({ d, scores[d], ok, ok });
		}
		std::sort(out.begin(), out.end(),
			[](const MoveScore& a, const MoveScore& b) { return a.score > b.score; });
		return out;
	}

	// A/B switch for the formation pruning experiment (off = original behaviour).
	void setIgnoreFormation(bool on) { std::lock_guard<std::mutex> lk(m_mutex); m_ignoreFormation = on; }
	// level 0 = expand both spawns everywhere, 1 = drop the 4-spawn on the leaf
	// layer, 2 = on the last two layers, and so on.
	// Heuristic weights. Rebuilding the 65536-entry table costs microseconds, so
	// this is safe to call between moves from a tuning harness.
	void setWeights(float wEmpty, float wMerges, float wMono, float wSum, float wSmooth) {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_wEmpty  = wEmpty;
		m_wMerges = wMerges;
		m_wMono   = wMono;
		m_wSum    = wSum;
		(void)wSmooth;   // kept in the signature so callers do not have to change
		rebuildScoreTable();
	}
	// Second weight set, used when at most `freeCells` empty cells remain.
	// freeCells = 0 disables it.
	void setPhase2(int freeCells, float wEmpty, float wMerges, float wMono, float wSum) {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_phaseFree = std::clamp(freeCells, 0, 15);
		m_w2Empty  = wEmpty;
		m_w2Merges = wMerges;
		m_w2Mono   = wMono;
		m_w2Sum    = wSum;
		rebuildScoreTable();
	}
	void phase2(int& freeCells, float out[4]) const {
		freeCells = m_phaseFree;
		out[0] = m_w2Empty; out[1] = m_w2Merges; out[2] = m_w2Mono; out[3] = m_w2Sum;
	}

	void weights(float out[5]) const {
		out[0] = m_wEmpty; out[1] = m_wMerges; out[2] = m_wMono;
		out[3] = m_wSum;   out[4] = 0.0f;   // smoothness removed: it never earned a non-zero weight
	}

	// Change the shape of the two magnitude curves (see m_sumExp). Rebuilds the
	// features and the score table, so call it before a game, not per move.
	void setShape(float sumExp, float monoExp) {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_sumExp  = std::clamp(sumExp, 1.0f, 8.0f);
		m_monoExp = std::clamp(monoExp, 1.0f, 8.0f);
		buildPowTables();
		rebuildFeatures();
		rebuildScoreTable();
	}
	void shape(float out[2]) const { out[0] = m_sumExp; out[1] = m_monoExp; }

	void setPrune4(int level) { std::lock_guard<std::mutex> lk(m_mutex); m_pruneDepth = level; }

	// Accept transposition entries that were searched deeper than the request.
	void setTtDeep(bool on) { std::lock_guard<std::mutex> lk(m_mutex); m_ttDeep = on; }
	void setTtWays(int n)   { std::lock_guard<std::mutex> lk(m_mutex); m_ttWays = (n >= 2 ? 2 : 1); }
	void setTtSlack(int n)  { std::lock_guard<std::mutex> lk(m_mutex); m_ttSlack = std::clamp(n, 0, 3); }

	// Attach a trained n-tuple network; nullptr puts the analytic heuristic back.
	void setNet(nt::Net* n) { std::lock_guard<std::mutex> lk(m_mutex); m_net = n; }

	// Resize the table (log2 of the slot count; 8 bytes per slot, so 2^24 = 128 MB).
	void setCacheBits(int bits) {
		if (m_cache->bits == bits) return;
		std::lock_guard<std::mutex> lk(m_mutex);
		allocCacheLocked(bits);
		cacheClear();
	}
	int cacheBits() const { return m_cache->bits; }

	// Upper bound on the boards the depth probe may materialise. Smaller = the
	// probe returns a shallower depth but costs proportionally less.
	void setProbeCap(std::size_t n) {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_probeCap = std::clamp<std::size_t>(n, 1024, (std::size_t)1 << 26);
		m_bfsBuf.assign(std::min(m_budget, m_probeCap), 0);
	}

	// Let several instances work on the same game and share one transposition
	// table, so parallel root moves still benefit from transpositions discovered
	// by the other workers.
	void shareCacheWith(const Solver4x4& other) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (other.m_cache->bits > 0) m_cache = other.m_cache;
	}

	// ---- Parallel entry points. Same depth and pruning semantics as
	// getAllMoveScores, just split per direction so the host can run the four
	// root moves on separate instances (sharing one transposition table).
	int searchDepth(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		for (int d = 0; d < 4; ++d)
			if (arr[d] != data && (m_ignoreFormation || intact(f, arr[d]))) validDirs[nValid++] = d;
		if (nValid == 0)
			for (int d = 0; d < 4; ++d)
				if (arr[d] != data) validDirs[nValid++] = d;
		if (nValid == 0) return 0;

		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validDirs[i]];

		const double t0 = nowMs();
		const int depth = bfsExpand(validBoards, nValid) + 1;
		m_prof.bfsMs += nowMs() - t0;
		m_prof.lastDepth = depth;
		m_prof.depthSum += (unsigned)depth;
		++m_prof.moves;
		return depth;
	}

	float scoreDirection(const BoardMatrix& board, int dir, int depth) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (dir < 0 || dir > 3 || depth < 1) return INVALID_SCORE;

		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		if (arr[dir] == data) return INVALID_SCORE;

		const Formation f = get(data);
		bool anyIntact = false;
		for (int d = 0; d < 4; ++d) {
			if (arr[d] != data && intact(f, arr[d])) { anyIntact = true; break; }
		}
		if (anyIntact && !intact(f, arr[dir])) return INVALID_SCORE;

		return scoreFrontier(arr[dir], (uint8_t)depth);
	}

	// How many spawn cells the root chance node of `dir` has, or -1 when the move
	// is illegal. The host uses this to cut the root into one task per spawn cell
	// instead of a fixed four slices: the subtrees of the four directions differ a
	// lot in size (they leave different numbers of free cells), so fixed slices
	// leave the longest task deciding the wall clock.
	int freeAfterMove(const BoardMatrix& board, int dir) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (dir < 0 || dir > 3) return -1;
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		if (arr[dir] == data) return -1;
		return (int)std_popcount(emptyPos(arr[dir]));
	}

	// One direction at a fixed depth, restricted to a slice of the root chance node.
	float scoreDirectionShard(const BoardMatrix& board, int dir, int depth, int shard, int nShards) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (dir < 0 || dir > 3 || depth < 1 || nShards < 1 || shard < 0 || shard >= nShards)
			return INVALID_SCORE;

		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		if (arr[dir] == data) return INVALID_SCORE;

		const Formation f = get(data);
		if (!m_ignoreFormation) {
			bool anyIntact = false;
			for (int d = 0; d < 4; ++d)
				if (arr[d] != data && intact(f, arr[d])) { anyIntact = true; break; }
			if (anyIntact && !intact(f, arr[dir])) return INVALID_SCORE;
		}
		const uint8_t d8 = (uint8_t)std::clamp(depth, 1, 63);
		const double t0 = nowMs();
		const float v = expectShard(arr[dir], d8, f, shard, nShards);
		m_prof.searchMs += nowMs() - t0;
		return v;
	}

	float evaluateBoard(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		return evaluate(encode(board));
	}

	std::vector<float> debugScoreAtDepth(const BoardMatrix& board, int depth) {
		std::lock_guard<std::mutex> lk(m_mutex);
		cacheClear();
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		const int officialOrder[4] = { 2, 0, 3, 1 };
		for (int oi = 0; oi < 4; ++oi) {
			const int d = officialOrder[oi];
			if (arr[d] != data && intact(f, arr[d])) validDirs[nValid++] = d;
		}
		if (nValid == 0)
			for (int oi = 0; oi < 4; ++oi) {
				const int d = officialOrder[oi];
				if (arr[d] != data) validDirs[nValid++] = d;
			}
		std::vector<float> scores(4, INVALID_SCORE);
		for (int i = 0; i < nValid; ++i) {
			const int d = validDirs[i];
			scores[d] = scoreFrontier(arr[d], (uint8_t)depth);
		}
		return scores;
	}

	std::vector<float> debugRawScores(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		cacheClear();
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const Formation f = get(data);
		const uint64_t arr[4] = { mv.up, mv.right, mv.down, mv.left };
		int validIdx[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		for (int oi = 0; oi < 4; ++oi) {
			const int d = oi;
			if (arr[d] != data && intact(f, arr[d])) validIdx[nValid++] = d;
		}
		if (nValid == 0)
			for (int oi = 0; oi < 4; ++oi) {
				const int d = oi;
				if (arr[d] != data) validIdx[nValid++] = d;
			}
		std::vector<float> scores(4, INVALID_SCORE);
		if (nValid == 0) return scores;
		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validIdx[i]];
		const int depth = bfsExpand(validBoards, nValid) + 1;
		for (int i = 0; i < nValid; ++i) {
			const int d = validIdx[i];
			scores[d] = scoreFrontier(arr[d], (uint8_t)depth);
		}
		return scores;
	}

	int debugDepth(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		for (int d = 0; d < 4; ++d)
			if (arr[d] != data && intact(f, arr[d])) validDirs[nValid++] = d;
		if (nValid == 0)
			for (int d = 0; d < 4; ++d)
				if (arr[d] != data) validDirs[nValid++] = d;
		if (nValid == 0) return 0;
		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validDirs[i]];
		return bfsExpand(validBoards, nValid) + 1;
	}

	std::vector<BoardMatrix> debugMoveBoards(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		std::vector<BoardMatrix> out;
		for (int d = 0; d < 4; ++d) {
			BoardMatrix b(4, std::vector<int>(4, 0));
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c) {
					const int rank = (int)((arr[d] >> ((3 - r) * 16 + 4 * c)) & 0xF);
					b[r][c] = (rank == 0) ? 0 : (1 << rank);
				}
			out.push_back(b);
		}
		return out;
	}

private:
	static constexpr uint64_t MULT_64 = 0xf1357aea2e62a9c5ULL;
	static constexpr uint64_t LANES   = 0x0F0F0F0F0F0F0F0FULL;
	static constexpr uint64_t ONES    = 0x0101010101010101ULL;
	static constexpr uint64_t GUARD   = 0x8080808080808080ULL;
	static constexpr std::size_t MAX_ROW  = 1ULL << 16;
	static constexpr int  MIN_RANK   = 9;
	static constexpr int  CACHE_BITS = 18;   // 4 MB: fits in L2, which measured 1.6x faster than 64 MB
	static constexpr std::size_t CACHE_SIZE = 1ULL << CACHE_BITS;

	static constexpr uint64_t FORMATIONS[24] = {
		0x00000000ff00fff0ULL, 0x0fff00ff00000000ULL, 0x0000000f00ff00ffULL, 0xff00ff00f0000000ULL,
		0xfff0ff0000000000ULL, 0x00ff00ff000f0000ULL, 0x0000f000ff00ff00ULL, 0x0000000000ff0fffULL,
		0xff00ff0000000000ULL, 0x00ff00ff00000000ULL, 0x00000000ff00ff00ULL, 0x0000000000ff00ffULL,
		0xff00f00000000000ULL, 0x00ff000f00000000ULL, 0x00000000f000ff00ULL, 0x00000000000f00ffULL,
		0xff00000000000000ULL, 0x000f000f00000000ULL, 0x00000000f000f000ULL, 0x00000000000000ffULL,
		0x000000000000ff00ULL, 0x00ff000000000000ULL, 0x00000000000f000fULL, 0xf000f00000000000ULL,
	};

	struct Formation {
		uint64_t even = 0, odd = 0, addend = 0, layout = 0;
	};

	static inline uint64_t atLeast(uint64_t board, int rank) {
		const uint64_t evens = board & LANES;
		const uint64_t odds  = (board >> 4) & LANES;
		const uint64_t addend = (0x80 - (uint64_t)rank) * ONES;
		const uint64_t even_hit = (evens + addend) & GUARD;
		const uint64_t odd_hit  = (odds  + addend) & GUARD;
		const uint64_t even_cells = (even_hit >> 3) - (even_hit >> 7);
		const uint64_t odd_cells  = (odd_hit  >> 3) - (odd_hit  >> 7);
		return even_cells | (odd_cells << 4);
	}

	static inline uint64_t largeTiles(uint64_t board) {
		return board & atLeast(board, MIN_RANK);
	}

	static inline bool intact(const Formation& f, uint64_t board) {
		if ((f.even | f.odd) == 0) return true;
		const uint64_t evens = board & LANES;
		const uint64_t odds  = (board >> 4) & LANES;
		return ((evens + f.addend) & f.even) == f.even &&
		((odds  + f.addend) & f.odd)  == f.odd;
	}

	static inline Formation update(const Formation& f, uint64_t board) {
		return (f.layout == largeTiles(board)) ? f : get(board);
	}

	static inline uint64_t maskedValue(uint64_t board, uint64_t cells) {
		uint64_t masked = board & cells;
		uint64_t total = 0;
		for (int i = 0; i < 16; ++i) {
			const uint64_t tile = masked & 0xF;
			masked >>= 4;
			total += (tile != 0) ? (1ULL << tile) : 0ULL;
		}
		return total;
	}

	static Formation get(uint64_t board) {
		const uint64_t layout = largeTiles(board);
		const Formation empty{ 0, 0, 0, layout };

		uint16_t present = 0, repeated = 0;
		uint64_t data = board;
		for (int i = 0; i < 16; ++i) {
			const uint8_t tile = (uint8_t)(data & 0xF);
			data >>= 4;
			if (tile < MIN_RANK) continue;
			const uint16_t bit = (uint16_t)(1u << tile);
			repeated |= present & bit;
			present  |= bit;
		}
		if (present == 0) return empty;

		int cutoff = std::min((int)std_countr_zero((unsigned)present) + 1, 15);

		if ((repeated >> cutoff) != 0) return empty;

		const uint64_t big    = atLeast(board, cutoff);
		const uint64_t addend = (0x80 - (uint64_t)cutoff) * ONES;

		Formation best = empty;
		uint64_t bestValue = 0;
		for (uint64_t cells : FORMATIONS) {
			if ((big & cells) != cells) continue;
			const uint64_t value = maskedValue(board, cells);
			if (value <= bestValue) continue;
			bestValue = value;
			best = { (cells & ONES) << 7, ((cells >> 4) & ONES) << 7, addend, layout };
		}
		return best;
	}

	static inline uint64_t emptyPos(uint64_t b) {
		b |= (b >> 2) & 0x3333333333333333ULL;
		b |= (b >> 1);
		return ~b & 0x1111111111111111ULL;
	}

	static inline uint64_t transpose(uint64_t x) {
		uint64_t b = (x ^ (x >> 12)) & 0x0000f0f00000f0f0ULL;
		x ^= b ^ (b << 12);
		b = (x ^ (x >> 24)) & 0x00000000ff00ff00ULL;
		x ^= b ^ (b << 24);
		return x;
	}

	static inline uint16_t reverse16(uint16_t x) {
		return (uint16_t)((x >> 12) | ((x >> 4) & 0x00f0) | ((x << 4) & 0x0f00) | (x << 12));
	}

	static inline uint64_t hashBits(uint64_t data, int bits) {
		return (data * MULT_64) >> (64 - bits);
	}

	// Packed line table: low 16 bits = slide left, high 16 bits = slide right.
	// One 32-bit load therefore serves both horizontal directions.
	uint32_t m_line[MAX_ROW];
	uint16_t m_forward[MAX_ROW];
	uint16_t m_reverse[MAX_ROW];

	static void moveLine(uint8_t* line) {
		int furthest = 4;
		bool merged = false;
		for (int i = 3; i >= 0; --i) {
			if (line[i] == 0) continue;
			if (!merged && furthest < 4 && line[i] == line[furthest]) {
				line[furthest] = (uint8_t)std::min(line[furthest] + 1, 15);   // u4 saturation: rank15+15 -> 15
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
	}

	void initMoveTable() {
		for (std::size_t row = 0; row < MAX_ROW; ++row) {
			uint8_t line[4] = {
				(uint8_t)(row & 0xF),
				(uint8_t)((row >> 4) & 0xF),
				(uint8_t)((row >> 8) & 0xF),
				(uint8_t)((row >> 12) & 0xF),
			};
			moveLine(line);
			const uint16_t moved = (uint16_t)(line[0] | (line[1] << 4) | (line[2] << 8) | (line[3] << 12));
			m_forward[(uint16_t)row] = moved;
			m_reverse[reverse16((uint16_t)row)] = reverse16(moved);
		}
		// Second pass: both directions for the same row index in one 32-bit word
		for (std::size_t row = 0; row < MAX_ROW; ++row)
			m_line[row] = (uint32_t)m_forward[row] | ((uint32_t)m_reverse[row] << 16);
	}

	struct Moves { uint64_t up, right, down, left; };

	__forceinline Moves getMoves(uint64_t data) const {
		const uint64_t dataT = transpose(data);
		uint64_t up = 0, right = 0, down = 0, left = 0;
		// Rows give the horizontal moves, columns (of the transposed board) the
		// vertical ones, so four packed lookups plus four more cover all eight.
		for (int idx = 0; idx < 4; ++idx) {
			const int src = (3 - idx) * 16;
			const int dst = src;   // keep the row order of the source board
			const uint32_t r = m_line[(uint16_t)(data  >> src)];
			const uint32_t c = m_line[(uint16_t)(dataT >> src)];
			left  |= (uint64_t)(r & 0xFFFFu) << dst;
			right |= (uint64_t)(r >> 16)      << dst;
			up    |= (uint64_t)(c & 0xFFFFu) << dst;
			down  |= (uint64_t)(c >> 16)      << dst;
		}
		up = transpose(up);
		down = transpose(down);
		return { up, right, down, left };
	}

	float m_scoreTable[MAX_ROW];
	// Second weight set, selected once the board has at most m_phaseFree empty
	// cells. The endgame is not the midgame: there the empty-cell term is nearly
	// constant and what decides the game is whether the chain stays intact, so a
	// separate weight vector is worth one more 256 KB table.
	float m_scoreTable2[MAX_ROW];
	float m_w2Empty  = 270.0f;
	float m_w2Merges = 700.0f;
	float m_w2Mono   = 47.0f;
	float m_w2Sum    = 11.0f;
	int   m_phaseFree = 0;      // 0 = phase 2 disabled

	// Row features, computed once. The score table is a linear combination of
	// them, so new weights can be applied in a single pass over 65536 rows
	// instead of rebuilding the features - that is what makes the weights
	// tunable at runtime (see bench/tune.sh).
	// Heap allocated: five 256 KB arrays on top of the score table would push the
	// object past the default 1 MB thread stack and kill the process at start-up.
	std::unique_ptr<float[]> m_fEmpty, m_fMerges, m_fMono, m_fSum;
	float m_sumPow[16];
	float m_monoPow[16];

	// Shape of the two magnitude curves: pow(rank, exponent). These are the only
	// smooth parameters in the evaluator - moving an exponent bends every entry of
	// the 65536-row table at once, whereas the five linear weights turned out to be
	// a plateau (see bench/tune.sh). Exposed through setShape()/--shape.
	float m_sumExp  = 3.5f;
	float m_monoExp = 4.0f;

	// Weights: empty cells, adjacent equal pairs, monotonicity, tile magnitude and
	// neighbouring difference. The last one is new relative to the reference
	// engine - its "merges" term only rewards exact equalities, so a row like
	// 2 4 8 16 is scored the same as 2 64 8 16 although the first one can still be
	// merged into a chain.
	float m_wEmpty  = 270.0f;
	float m_wMerges = 700.0f;
	float m_wMono   = 47.0f;
	float m_wSum    = 11.0f;
	void initHeuristic() {
		m_fEmpty .reset(new float[MAX_ROW]);
		m_fMerges.reset(new float[MAX_ROW]);
		m_fMono  .reset(new float[MAX_ROW]);
		m_fSum   .reset(new float[MAX_ROW]);

		buildPowTables();
		rebuildFeatures();
		rebuildScoreTable();
	}

	// Tile magnitude is pow(rank, exponent) with rank = log2(tile), so the curve is
	// the same for every level and only the exponent decides how strongly a big
	// tile is preferred over many small ones.
	void buildPowTables() {
		m_sumPow[0] = 0; m_monoPow[0] = 0;
		for (int idx = 1; idx < 16; ++idx) {
			const float fidx = (float)idx;
			m_sumPow[idx]  = std::exp2(std::log2(fidx) * m_sumExp);
			m_monoPow[idx] = std::exp2(std::log2(fidx) * m_monoExp);
		}
	}

	// Row features, recomputed from the current pow tables. 65536 iterations, so
	// this is cheap enough to call between moves while tuning.
	void rebuildFeatures() {
		for (std::size_t row = 0; row < MAX_ROW; ++row) {
			const uint8_t line[4] = {
				(uint8_t)(row & 0xF),
				(uint8_t)((row >> 4) & 0xF),
				(uint8_t)((row >> 8) & 0xF),
				(uint8_t)((row >> 12) & 0xF),
			};

			float sum = 0;
			int   empty = 0, merges = 0;
			int   prev = 0, counter = 0;

			for (int i = 0; i < 4; ++i) {
				const int rank = line[i];
				sum += m_sumPow[rank];
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
				if (i > 0) {
					const int d = (int)line[i] - (int)line[i - 1];
				}
			}
			if (counter > 0) merges += 1 + counter;

			float monoLeft = 0, monoRight = 0;
			for (int i = 1; i < 4; ++i) {
				const float l = m_monoPow[line[i - 1]];
				const float r = m_monoPow[line[i]];
				if (line[i - 1] > line[i]) monoLeft  += l - r;
				else                      monoRight += r - l;
			}

			m_fEmpty[row]  = (float)empty;
			m_fMerges[row] = (float)merges;
			m_fMono[row]   = std::min(monoLeft, monoRight);
			m_fSum[row]    = sum;
		}
	}

	void rebuildScoreTable() {
		for (std::size_t row = 0; row < MAX_ROW; ++row) {
			m_scoreTable[row] = 200000.0f
				+ m_wEmpty  * m_fEmpty[row]
				+ m_wMerges * m_fMerges[row]
				- m_wMono   * m_fMono[row]
				- m_wSum    * m_fSum[row];
			m_scoreTable2[row] = 200000.0f
				+ m_w2Empty  * m_fEmpty[row]
				+ m_w2Merges * m_fMerges[row]
				- m_w2Mono   * m_fMono[row]
				- m_w2Sum    * m_fSum[row];
		}
	}

	__forceinline float evaluate(uint64_t data) const {
		// A trained 6-cell network replaces the eight 4-cell line tables when one is
		// attached. Two patterns of the net span a 2x3 block, which is where the
		// linear table is blind: it cannot tell "this block has no way out" from
		// "this block is fine", and that is the judgement the endgame needs.
		if (m_net) return m_net->valueFromBits(data);
		const uint64_t dataT = transpose(data);
		const float* tbl = m_scoreTable;
		if (m_phaseFree > 0 && std_popcount(emptyPos(data)) <= m_phaseFree) tbl = m_scoreTable2;
		float score = 0;
		for (int idx = 0; idx < 4; ++idx) {
			const int shift = idx * 16;
			const uint16_t row = (uint16_t)(data >> shift);
			const uint16_t col = (uint16_t)(dataT >> shift);
			score += tbl[row] + tbl[col];
		}
		return score;
	}

	// Transposition table: one 8-byte slot per bucket, shared by all workers.
	// Layout is [24-bit score | 40-bit key], so a lookup is a single relaxed atomic
	// load and a store is a single relaxed atomic store - no locks, no torn values.
	//
	// The key must not repeat the bits used for the bucket index. With 2^24 buckets
	// the index owns the top 24 bits of hash(bucket), so a 32-bit key taken from
	// the top of the hash would leave only 8 bits of disagreement inside a bucket:
	// a 1-in-256 false hit, i.e. a wrong score on ~0.4% of all lookups. That is
	// enough to make multi-threaded play non-reproducible (different eviction order
	// -> different false hits) and to hand the search occasional garbage values.
	// 40 low bits give an independent tag with a ~1e-5 collision rate.
	// One slot per bucket: 64-bit key plus the full 32-bit float score, padded to
	// 16 bytes so a single aligned SSE load/store moves it atomically.
	//
	// A compact [24-bit score | 40-bit key] word was tried first, but 2^24 buckets
	// with a 40-bit tag means 2^24/2^40 ~ 1.5e-5 chance that a bucket holds a
	// different board with a matching tag. At ~1.4M lookups per move that is ~20
	// wrong scores per move, and it made multi-threaded play non-reproducible,
	// because thread interleaving changes which entries survive. With a 64-bit key
	// the same computation gives ~1e-13, i.e. never in practice.
	struct alignas(16) Slot {
		uint64_t key;
		uint32_t bits;    // float score, bit pattern
		uint32_t depth;   // plies this value was searched to; 0 = empty slot
	};

	struct Cache {
		std::unique_ptr<Slot[]> slots;
		std::size_t size = 0;
		int         bits = 0;   // log2(slots); resizable at runtime
	};

	static inline uint64_t keyOf(uint64_t board) {
		uint64_t mixed = board * 0xD6E8FEB86659FD93ULL;
		mixed ^= mixed >> 29;
		return mixed ? mixed : 1;   // 0 means "empty slot"
	}

	__forceinline Slot loadSlot(std::size_t i) const {
		const Slot* p = &m_cache->slots[i];
#if defined(_MSC_VER)
		const __m128i raw = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
		Slot out;
		_mm_store_si128(reinterpret_cast<__m128i*>(&out), raw);
		return out;
#else
		Slot out;
		std::memcpy(&out, p, sizeof out);
		return out;
#endif
	}

	std::shared_ptr<Cache> m_cache = std::make_shared<Cache>();

	__forceinline void storeSlot(std::size_t i, Slot v) const {
		Slot* p = &m_cache->slots[i];
#if defined(_MSC_VER)
		_mm_store_si128(reinterpret_cast<__m128i*>(p),
		                _mm_load_si128(reinterpret_cast<const __m128i*>(&v)));
#else
		std::memcpy(p, &v, sizeof v);
#endif
	}

	void allocCacheLocked(int bits) {
		bits = std::clamp(bits, 14, 26);
		m_cache->slots.reset(new Slot[(std::size_t)1 << bits]);
		m_cache->size = (std::size_t)1 << bits;
		m_cache->bits = bits;
	}

	__forceinline void cacheClear() {
		auto& c = *m_cache;
		std::memset(c.slots.get(), 0, c.size * sizeof(Slot));
	}

	// Two slots per bucket, deepest-wins replacement.
	//
	// With one slot per hash the table is overwritten about four times per move
	// (a million nodes into 262k slots), so a deep entry survives only by luck and
	// the accept-if-deeper policy - the whole point of the table - almost never
	// pays. Giving each bucket two slots and evicting the shallower one keeps the
	// entries that can answer a deep request, at identical memory.
	__forceinline void cacheInsert(uint64_t board, uint8_t depth, float score) {
		const uint64_t k = keyOf(board);
		Slot v;
		v.key = k;
		std::memcpy(&v.bits, &score, 4);
		v.depth = depth;

		if (m_ttWays < 2) {
			const std::size_t i = hashBits(board, m_cache->bits);
			const Slot old = loadSlot(i);
			if (old.key == k && old.depth >= (uint32_t)depth) return;
			storeSlot(i, v);
			return;
		}

		const std::size_t base = hashBits(board, m_cache->bits - 1) << 1;
		const Slot s0 = loadSlot(base);
		const Slot s1 = loadSlot(base + 1);
		if (s0.key == k && s0.depth >= (uint32_t)depth) return;
		if (s1.key == k && s1.depth >= (uint32_t)depth) return;

		std::size_t victim;
		if      (s0.key == k)  victim = base;
		else if (s1.key == k)  victim = base + 1;
		else if (s0.depth == 0) victim = base;
		else if (s1.depth == 0) victim = base + 1;
		else victim = (s0.depth <= s1.depth) ? base : base + 1;
		storeSlot(victim, v);
	}

	// A stored value is accepted when it was searched at least as deep as the
	// request. This is the reference engine's policy and it is what makes the
	// table pay: with an exact-depth match almost every lookup misses, because a
	// board is reached again at a different remaining depth far more often than at
	// the same one. Values are therefore approximations, not exact, which is the
	// price of the extra effective depth (--ttshallow turns it off for an A/B).
	__forceinline bool cacheQuery(uint64_t board, uint8_t depth, float& out) const {
		const uint64_t k = keyOf(board);
		if (m_ttWays < 2) {
			const Slot v = loadSlot(hashBits(board, m_cache->bits));
			if (v.key != k) return false;
			if (!m_ttDeep) { if (v.depth != (uint32_t)depth) return false; }
			else           { if (v.depth + (uint32_t)m_ttSlack < (uint32_t)depth) return false; }
			std::memcpy(&out, &v.bits, 4);
			return true;
		}
		const std::size_t base = hashBits(board, m_cache->bits - 1) << 1;
		const Slot s0 = loadSlot(base);
		const Slot s1 = loadSlot(base + 1);
		const Slot* hit = nullptr;
		if (s0.key == k) hit = &s0;
		if (s1.key == k && (!hit || s1.depth > hit->depth)) hit = &s1;
		if (!hit) return false;
		if (!m_ttDeep) { if (hit->depth != (uint32_t)depth) return false; }
		else           { if (hit->depth + (uint32_t)m_ttSlack < (uint32_t)depth) return false; }
		std::memcpy(&out, &hit->bits, 4);
		return true;
	}

	std::vector<uint64_t> m_bfsBuf;
	std::vector<uint32_t> m_radixCount;
	std::size_t m_budget = 0;
	std::size_t m_probeCap = (std::size_t)1 << 25;   // hard cap on the depth probe

	__forceinline static void radixSort(uint64_t* src, uint64_t* dst,
		std::size_t len, uint32_t* count) {
			for (int pass = 0; pass < 2; ++pass) {
				std::memset(count, 0, 65536 * sizeof(uint32_t));
				const uint32_t shift = (uint32_t)pass * 16;
				for (std::size_t i = 0; i < len; ++i)
					++count[(((uint32_t)hashBits(src[i], 32)) >> shift) & 0xFFFF];
				uint32_t acc = 0;
				for (int i = 0; i < 65536; ++i) {
					const uint32_t old = acc;
					acc += count[i];
					count[i] = old;
				}
				for (std::size_t i = 0; i < len; ++i) {
					const uint32_t radix = (((uint32_t)hashBits(src[i], 32)) >> shift) & 0xFFFF;
					dst[count[radix]] = src[i];
					++count[radix];
				}
				std::swap(src, dst);
			}
		}

	int bfsExpand(const uint64_t* initial, int initialLen) {
		const std::size_t half = std::min(m_budget, m_probeCap) / 2;
		if (half == 0) return 0;
		const std::size_t cap = std::min((std::size_t)initialLen, half);
		uint64_t* cur = m_bfsBuf.data();
		uint64_t* nxt = m_bfsBuf.data() + half;
		uint32_t* count = m_radixCount.data();

		std::memcpy(cur, initial, cap * sizeof(uint64_t));
		std::size_t curLen = cap;
		int depth = 0;
		std::size_t prevLen = 1;
		const int MAX_DEPTH = 63;

		while (curLen > 0 && depth < MAX_DEPTH) {
			const uint64_t est = (uint64_t)curLen * curLen / prevLen;
			if (est > (uint64_t)half * 2) break;

			radixSort(cur, nxt, curLen, count);

			std::size_t nextLen = 0;
			uint64_t last = 0;
			bool full = false;

			m_prof.bfsNodes += curLen;
			for (std::size_t i = 0; i < curLen; ++i) {
				const uint64_t nb = cur[i];
				if (last == nb) continue;
				last = nb;

				const Formation f = get(nb);
				uint64_t mask = emptyPos(nb);
				while (mask != 0) {
					const uint64_t tile = mask & (~mask + 1);
					mask ^= tile;
					const uint64_t sp2 = last | tile;
					const uint64_t sp4 = last | (tile << 1);

					const Moves m2 = getMoves(sp2);
					const Moves m4 = getMoves(sp4);
					const uint64_t a2[4] = { m2.up, m2.right, m2.down, m2.left };
					const uint64_t a4[4] = { m4.up, m4.right, m4.down, m4.left };

					for (int k = 0; k < 4; ++k) {
						if (a2[k] != sp2 && intact(f, a2[k])) {
							if (nextLen == half) { full = true; break; }
							nxt[nextLen++] = a2[k];
						}
						if (a4[k] != sp4 && intact(f, a4[k])) {
							if (nextLen == half) { full = true; break; }
							nxt[nextLen++] = a4[k];
						}
					}
					if (full) break;
				}
				if (full) break;
			}
			if (full) break;

			std::swap(cur, nxt);
			prevLen = curLen;
			curLen = nextLen;
			depth += 1;
		}

		return depth;
	}

	bool m_ignoreFormation = false;
	int   m_pruneDepth  = 0;
	bool  m_ttDeep      = true;
	int   m_ttWays      = 1;    // slots per hash bucket; 2 measured no gain
	int   m_ttSlack     = 0;    // accept entries up to N plies shallower
	nt::Net* m_net      = nullptr;   // non-null: score with the trained network

	std::mutex m_mutex;

	__forceinline float expectNode(uint64_t board, uint8_t depth, const Formation& f,
	                               float prob) {
		++m_prof.searchNodes;
		if (depth == 0) return evaluate(board);

		float cached = 0;
		if (cacheQuery(board, depth, cached)) {
			++m_prof.cutNodes;
			return cached;
		}

		uint64_t mask = emptyPos(board);
		const float total = (float)std_popcount(mask);
		const float w2 = 0.9f / total;
		const float w4 = 0.1f / total;
		float score = 0;

		const Formation nf = update(f, board);

		// At the last expansion layer the 4-spawn child is left out: it is the
		// dominant share of the leaf cost and carries only 10% of the probability,
		// so dropping it roughly halves the node count for a tiny accuracy loss.
		const bool skip4 = depth <= m_pruneDepth;
		while (mask != 0) {
			const uint64_t tile = mask & (~mask + 1);
			mask ^= tile;
			score += w2 * maxNode(board | tile, depth, nf, prob * w2);
			if (!skip4) score += w4 * maxNode(board | (tile << 1), depth, nf, prob * w4);
		}

		cacheInsert(board, depth, score);
		return score;
	}

	__forceinline float maxNode(uint64_t board, uint8_t depth, const Formation& f, float prob) {
		++m_prof.searchNodes;
		const Moves m = getMoves(board);
		const uint64_t arr[4] = { m.up, m.right, m.down, m.left };
		// Formation pruning is a branching-factor shortcut: a move that breaks the
		// lane parity of the position is skipped, at every ply and without a
		// fallback. That is fine while there is room, but in a crowded endgame the
		// only move that restructures the board is often the one that breaks the
		// formation, and the node then returns 0 (= "this position is worth
		// nothing"), which the search reads as certain death. --nointact removes
		// the filter from the tree as well, which is what makes it a real A/B.
		const bool noFormation = m_ignoreFormation;
		float maxScore = noFormation ? -1e30f : 0.0f;
		for (int k = 0; k < 4; ++k) {
			const uint64_t nb = arr[k];
			if (nb == board) continue;
			if (!noFormation && !intact(f, nb)) continue;
			maxScore = std::max(maxScore, expectNode(nb, (uint8_t)(depth - 1), f, prob));
		}
		return maxScore;
	}

	__forceinline float scoreFrontier(uint64_t board, uint8_t depth) {
		const double t0 = nowMs();
		const float v = expectNode(board, depth, get(board), 1.0f);
		m_prof.searchMs += nowMs() - t0;
		return v;
	}

	// Partial expectimax value of one root direction. The first chance node is cut
	// into nShards slices; slice i only expands every nShards-th empty cell, so the
	// sum over all slices equals the unsharded score. Used to fan the four root
	// moves out over several threads without changing the algorithm.
	__forceinline float expectShard(uint64_t board, uint8_t depth, const Formation& f,
	                                int shard, int nShards) {
		++m_prof.searchNodes;
		if (depth == 0) return evaluate(board);

		uint64_t mask = emptyPos(board);
		const float total = (float)std_popcount(mask);
		const float w2 = 0.9f / total;
		const float w4 = 0.1f / total;
		float score = 0;

		const Formation nf = update(f, board);
		const bool skip4 = depth <= m_pruneDepth;
		int idx = 0;
		while (mask != 0) {
			const uint64_t tile = mask & (~mask + 1);
			mask ^= tile;
			if ((idx++ % nShards) != shard) continue;
			score += w2 * maxNode(board | tile, depth, nf, w2);
			if (!skip4) score += w4 * maxNode(board | (tile << 1), depth, nf, w4);
		}
		return score;
	}

	static uint64_t encode(const BoardMatrix& board) {
		uint64_t data = 0;
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) {
				int v = board[r][c];
				int logVal = 0;
				if (v <= 0) {
					logVal = 0;
				} else if (v >= (1 << 28)) {
					logVal = 15;
				} else {
					while (v > 1) { v >>= 1; ++logVal; }
					if (logVal > 15) logVal = 15;   // 65536+ -> rank15
				}
				data |= (uint64_t)logVal << ((3 - r) * 16 + 4 * c);
			}
		}
		return data;
	}
};

// Clean up macros to avoid polluting the global namespace
#undef std_bit_cast
#undef std_countr_zero
#undef std_popcount

#endif

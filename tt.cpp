#include "tt.h"
#include "types.h"

// bestMove uses only 29 bits in this engine (see move.h layout), so we can store
// TT flag in bits 29..30 without losing information.
static inline uint32_t pack_move_and_flag(uint32_t bestMove, uint8_t flag) {
  return (bestMove & 0x1FFFFFFFu) | ((uint32_t)(flag & 0x3u) << 29);
}

static inline void unpack_move_and_flag(uint32_t packed, uint32_t& bestMove, uint8_t& flag) {
  bestMove = packed & 0x1FFFFFFFu;
  flag = (uint8_t)((packed >> 29) & 0x3u);
}

static inline uint64_t pack_data(uint32_t bestMove, int score, int depth, uint8_t flag, uint8_t gen) {
  uint32_t lo = pack_move_and_flag(bestMove, flag);
  uint16_t sc = (uint16_t)(int16_t)score;
  uint8_t d = (uint8_t)(int8_t)depth;
  uint64_t hi = (uint64_t)sc | ((uint64_t)d << 16) | ((uint64_t)gen << 24);
  return (uint64_t)lo | (hi << 32);
}

static inline void unpack_data(uint64_t data, TTEntry& out) {
  uint32_t lo = (uint32_t)(data & 0xFFFFFFFFu);
  uint32_t bm; uint8_t fl;
  unpack_move_and_flag(lo, bm, fl);
  out.bestMove = bm;
  out.flag = fl;

  uint32_t hi = (uint32_t)(data >> 32);
  out.score = (int16_t)(hi & 0xFFFFu);
  out.depth = (int8_t)((hi >> 16) & 0xFFu);
  out.gen   = (uint8_t)((hi >> 24) & 0xFFu);
}

void TT::resize_mb(int mb) {
  size_t bytes = (size_t)mb * 1024ULL * 1024ULL;
  size_t n = bytes / sizeof(TTBucket);
  if (n < 1) n = 1;
  t.clear();
  t.resize(n);
}

void TT::clear() {
  if (!t.empty()) {
    for (auto& b : t) {
      for (auto& e : b.e) {
        e.key.store(0, std::memory_order_relaxed);
        e.data.store(0, std::memory_order_relaxed);
      }
    }
  }
  gen = 1;
}

bool TT::probe(uint64_t key, TTEntry& out) const {
  if (t.empty()) return false;
  const TTBucket& b = t[key % t.size()];
  for (const auto& e : b.e) {
    uint64_t k = e.key.load(std::memory_order_acquire);
    if (k == key) {
      out.key = key;
      unpack_data(e.data.load(std::memory_order_relaxed), out);
      return true;
    }
  }
  return false;
}

int TT::hashfull() const {
  if (t.empty()) return 0;
  // Sample first N buckets (UCI expects a quick estimate)
  const size_t buckets = std::min<size_t>(t.size(), 1000);
  int filled = 0;
  for (size_t i = 0; i < buckets; i++) {
    for (const auto& e : t[i].e) {
      uint64_t k = e.key.load(std::memory_order_relaxed);
      if (!k) continue;
      TTEntry tmp;
      tmp.key = k;
      unpack_data(e.data.load(std::memory_order_relaxed), tmp);
      if (tmp.gen == gen) filled++;
    }
  }
  const int total = int(buckets * 4);
  return total ? (filled * 1000) / total : 0;
}

static inline int age(uint8_t now, uint8_t then) {
  return (int)((uint8_t)(now - then)); // wrap-safe
}

void TT::store(uint64_t key, int depth, int score, uint8_t flag, uint32_t bestMove) {
  if (t.empty()) return;
  TTBucket& b = t[key % t.size()];

  // Pre-pack once.
  const uint64_t newData = pack_data(bestMove, score, depth, flag, gen);

  // If key exists, replace if deeper or if improving bound quality
  for (auto& e : b.e) {
    uint64_t k = e.key.load(std::memory_order_acquire);
    if (k == key) {
      TTEntry cur;
      cur.key = k;
      unpack_data(e.data.load(std::memory_order_relaxed), cur);

      // Replace if deeper, exact, or entry is from older generation.
      if (depth > cur.depth || flag == TT_EXACT || cur.gen != gen) {
        e.data.store(newData, std::memory_order_relaxed);
        e.key.store(key, std::memory_order_release);
      } else if (bestMove && !cur.bestMove) {
        // Keep existing score/depth but add a best move if missing.
        uint64_t patched = pack_data(bestMove, cur.score, cur.depth, cur.flag, cur.gen);
        e.data.store(patched, std::memory_order_relaxed);
        e.key.store(key, std::memory_order_release);
      }
      return;
    }
  }

  // Choose a victim: empty first, otherwise "worst" by depth/age/bound
  int victim = 0;
  int bestScore = 1e9;
  for (int i=0;i<4;i++) {
    const auto& e = b.e[i];
    uint64_t k = e.key.load(std::memory_order_relaxed);
    if (k == 0) { victim = i; bestScore = -1e9; break; }
    TTEntry cur;
    cur.key = k;
    unpack_data(e.data.load(std::memory_order_relaxed), cur);
    int a = age(gen, cur.gen);
    int s = (int)cur.depth - 2*a;
    if (cur.flag != TT_EXACT) s -= 1; // exact entries slightly protected
    if (s < bestScore) { bestScore = s; victim = i; }
  }

  auto& e = b.e[victim];
  e.data.store(newData, std::memory_order_relaxed);
  e.key.store(key, std::memory_order_release);
}

static constexpr int MATE = SCORE_INF;

int TT::pack_score(int score, int ply) const {
  // Store mates as "mate in N" so closer mates are preferred.
  if (score > SCORE_MATE - 1000) return score + ply;
  if (score < -SCORE_MATE + 1000) return score - ply;
  return score;
}

int TT::unpack_score(int score, int ply) const {
  if (score > SCORE_MATE - 1000) return score - ply;
  if (score < -SCORE_MATE + 1000) return score + ply;
  return score;
}

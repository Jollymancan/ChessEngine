#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <cstring>
#include <atomic>

enum : uint8_t { TT_ALPHA=0, TT_BETA=1, TT_EXACT=2 };

struct TTEntry {
  uint64_t key = 0;
  uint32_t bestMove = 0;
  int16_t score = 0;
  int8_t depth = 0;
  uint8_t flag = TT_ALPHA;
  uint8_t gen = 0;   // generation (ageing)
  uint8_t _pad = 0;  // keep alignment simple
};

// Thread-safe packed TT entry.
// We avoid UB data races by only touching atomics.
//
// Layout of packed 'data' (64-bit):
//  - low  32 bits: bestMove with TT flag stored in bits 29..30 (move uses only 29 bits)
//  - high 32 bits: score (16), depth (8), gen (8)
struct TTEntryPacked {
  std::atomic<uint64_t> key{0};
  std::atomic<uint64_t> data{0};

  TTEntryPacked() = default;

  // std::atomic is not copyable by default. We make TT entries movable/copyable
  // by copying the contained values, which is fine for a transposition table.
  TTEntryPacked(const TTEntryPacked& o)
      : key(o.key.load(std::memory_order_relaxed)),
        data(o.data.load(std::memory_order_relaxed)) {}

  TTEntryPacked& operator=(const TTEntryPacked& o) {
    key.store(o.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
    data.store(o.data.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }

  TTEntryPacked(TTEntryPacked&& o) noexcept : TTEntryPacked(o) {}
  TTEntryPacked& operator=(TTEntryPacked&& o) noexcept { return (*this = o); }
};

struct TTBucket {
  std::array<TTEntryPacked, 4> e{};

  TTBucket() = default;
  TTBucket(const TTBucket& o) : e(o.e) {}
  TTBucket& operator=(const TTBucket& o) { e = o.e; return *this; }
  TTBucket(TTBucket&& o) noexcept : TTBucket(o) {}
  TTBucket& operator=(TTBucket&& o) noexcept { return (*this = o); }
};

struct TT {
  std::vector<TTBucket> t;
  uint8_t gen = 1;

  void resize_mb(int mb);
  void clear();

  // Call once per new root search to age entries (no need to clear)
  inline void new_search() { gen = uint8_t(gen + 1); if (gen == 0) gen = 1; }

  bool probe(uint64_t key, TTEntry& out) const;
  void store(uint64_t key, int depth, int score, uint8_t flag, uint32_t bestMove);

  // UCI: hashfull in permill (0..1000)
  int hashfull() const;

  // mate distance packing (minimal; keeps it extendable)
  int pack_score(int score, int ply) const;
  int unpack_score(int score, int ply) const;
};

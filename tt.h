#pragma once
#include <cstdint>
#include <vector>

enum : uint8_t { TT_ALPHA=0, TT_BETA=1, TT_EXACT=2 };

struct TTEntry {
  uint64_t key = 0;
  uint32_t bestMove = 0;
  int16_t score = 0;
  int8_t depth = 0;
  uint8_t flag = TT_ALPHA;
};

struct TT {
  std::vector<TTEntry> t;

  void resize_mb(int mb);
  void clear();

  bool probe(uint64_t key, TTEntry& out) const;
  void store(uint64_t key, int depth, int score, uint8_t flag, uint32_t bestMove);

  // mate distance packing (minimal; keeps it extendable)
  int pack_score(int score, int ply) const;
  int unpack_score(int score, int ply) const;
};

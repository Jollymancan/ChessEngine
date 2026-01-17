#include "tt.h"
#include "position.h"
#include "bitboard.h"
#include <cstring>

static uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// slow zobrist (good enough to start, easy to convert to incremental later)
uint64_t zobrist_key_slow(const Position& pos) {
  static bool inited = false;
  static uint64_t ZP[12][64];
  static uint64_t ZSide;
  static uint64_t ZCastle[16];
  static uint64_t ZEP[9]; // file 0..7, plus 8 for none

  if (!inited) {
    uint64_t seed = 123456789ULL;
    for (int pc=0; pc<12; pc++)
      for (int sq=0; sq<64; sq++)
        ZP[pc][sq] = splitmix64(seed);
    ZSide = splitmix64(seed);
    for (int i=0;i<16;i++) ZCastle[i] = splitmix64(seed);
    for (int i=0;i<9;i++) ZEP[i] = splitmix64(seed);
    inited = true;
  }

  uint64_t k = 0;
  for (int sq=0;sq<64;sq++){
    int c = pos.board[sq];
    if (c != 12) k ^= ZP[c][sq];
  }
  if (pos.stm == BLACK) k ^= ZSide;
  k ^= ZCastle[pos.castling & 15];

  int epFile = 8;
  if (pos.epSq != -1) epFile = pos.epSq & 7;
  k ^= ZEP[epFile];

  return k;
}

void TT::resize_mb(int mb) {
  size_t bytes = (size_t)mb * 1024ULL * 1024ULL;
  size_t n = bytes / sizeof(TTEntry);
  if (n < 1) n = 1;
  t.assign(n, TTEntry{});
}

void TT::clear() {
  std::memset(t.data(), 0, t.size() * sizeof(TTEntry));
}

bool TT::probe(uint64_t key, TTEntry& out) const {
  if (t.empty()) return false;
  const TTEntry& e = t[key % t.size()];
  if (e.key == key) { out = e; return true; }
  return false;
}

void TT::store(uint64_t key, int depth, int score, uint8_t flag, uint32_t bestMove) {
  if (t.empty()) return;
  TTEntry& e = t[key % t.size()];
  if (e.key != key || depth >= e.depth) {
    e.key = key;
    e.depth = (int8_t)depth;
    e.score = (int16_t)score;
    e.flag = flag;
    e.bestMove = bestMove;
  }
}

int TT::pack_score(int score, int /*ply*/) const { return score; }
int TT::unpack_score(int score, int /*ply*/) const { return score; }

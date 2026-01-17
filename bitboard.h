#pragma once
#include "types.h"

#if defined(_MSC_VER)
  #include <intrin.h>
  #include <immintrin.h>
#endif

inline constexpr U64 sq_bb(int sq) { return 1ULL << sq; }
inline int file_of(int sq) { return sq & 7; }
inline int rank_of(int sq) { return sq >> 3; }

inline int ctz64(U64 x) {
#if defined(_MSC_VER)
  unsigned long idx;
  _BitScanForward64(&idx, x);
  return (int)idx;
#else
  return __builtin_ctzll(x);
#endif
}

inline int popcount64(U64 x) {
#if defined(_MSC_VER)
  return (int)__popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

inline int pop_lsb(U64& bb) {
  int sq = ctz64(bb);
  bb &= (bb - 1);
  return sq;
}

#include "zobrist.h"

static uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

uint64_t ZP[12][64];
uint64_t ZSide;
uint64_t ZCastle[16];
uint64_t ZEP[9];

void zobrist_init() {
  static bool inited = false;
  if (inited) return;

  uint64_t seed = 123456789ULL;
  for (int pc=0; pc<12; pc++)
    for (int sq=0; sq<64; sq++)
      ZP[pc][sq] = splitmix64(seed);

  ZSide = splitmix64(seed);
  for (int i=0;i<16;i++) ZCastle[i] = splitmix64(seed);
  for (int i=0;i<9;i++)  ZEP[i] = splitmix64(seed);

  inited = true;
}

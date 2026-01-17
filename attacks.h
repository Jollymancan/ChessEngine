#pragma once
#include "types.h"

struct Attacks {
  U64 pawn[2][64]{};
  U64 knight[64]{};
  U64 king[64]{};

  void init();
};

extern Attacks ATK;

U64 rook_attacks(int sq, U64 occ);
U64 bishop_attacks(int sq, U64 occ);

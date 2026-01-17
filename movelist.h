#pragma once
#include "move.h"

struct MoveList {
  Move moves[256];
  int size = 0;
  inline void push(Move m) { moves[size++] = m; }
};

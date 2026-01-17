#pragma once
#include <algorithm>
#include "types.h"
#include "move.h"
#include "movelist.h"

constexpr int EMPTY_CODE = 12; // board[] entry for empty

inline int code(Color c, Piece p) { return (int)c*6 + (int)p; }
inline Piece code_piece(int c) { return (Piece)(c % 6); }


struct Undo {
  uint8_t castling;
  int epSq;
  int capturedCode; // EMPTY_CODE if none
};

struct Position {
  U64 bb[2][6]{};
  U64 occ[2]{};
  U64 occAll = 0;

  int board[64];
  Color stm = WHITE;
  uint8_t castling = 0;
  int epSq = NO_SQ;
  int kingSq[2]{NO_SQ, NO_SQ};

  Position();

  void rebuild_occ();
  bool is_attacked(int sq, Color by) const;

  void gen_pseudo(MoveList& ml) const;

  void make(Move m, Undo& u);
  void unmake(Move m, const Undo& u);
};



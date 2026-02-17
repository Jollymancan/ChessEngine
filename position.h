#pragma once
#include <algorithm>
#include <iterator>
#include <vector>
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
  U64 key;
  U64 pawnKey;
  U64 occ[2];
  uint16_t halfmoveClock;
  uint16_t fullmoveNumber;
};

struct Position {
  U64 bb[2][6]{};
  U64 occ[2]{};
  U64 occAll = 0;

  // Incremental Zobrist key (piece-square + side-to-move + castling + ep-file)
  U64 key = 0;

  // Incremental pawn-only Zobrist key (for pawn hash)
  U64 pawnKey = 0;

  int board[64];
  Color stm = WHITE;
  uint8_t castling = 0;
  int epSq = NO_SQ;
  int kingSq[2]{NO_SQ, NO_SQ};

  // Draw / move counters from FEN (halfmoves since last pawn move/capture, and fullmove number)
  uint16_t halfmoveClock = 0;
  uint16_t fullmoveNumber = 1;

  // Key history for true repetition detection (root/game line only; search uses its own stack too)
  std::vector<U64> gameKeys;

  Position();

  void rebuild_occ();
  void rebuild_key();
  void rebuild_pawn_key();
  bool is_attacked(int sq, Color by) const;

  void gen_pseudo(MoveList& ml) const;


  // Repetition helpers (game history)
  void reset_game_history();
  void push_game_key();
  int repetition_count() const;
  bool is_draw_50move() const { return halfmoveClock >= 100; }

  void make(Move m, Undo& u);
  void unmake(Move m, const Undo& u);

  // Null move (for search pruning)
  void make_null(Undo& u);
  void unmake_null(const Undo& u);
};



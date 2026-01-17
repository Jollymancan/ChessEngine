#pragma once
#include <cstdint>

using U64 = uint64_t;

enum Color : int { WHITE=0, BLACK=1 };
enum Piece : int { PAWN=0, KNIGHT=1, BISHOP=2, ROOK=3, QUEEN=4, KING=5, NO_PIECE=6 };

constexpr int NO_SQ = -1;

inline constexpr Color operator!(Color c) { return c == WHITE ? BLACK : WHITE; }

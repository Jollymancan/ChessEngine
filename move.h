#pragma once
#include <cstdint>
#include "types.h"

using Move = uint32_t;
// [ flags:8 | promo:3 | cap:3 | piece:3 | to:6 | from:6 ]

enum MoveFlags : uint8_t {
  MF_NONE     = 0,
  MF_EP       = 1 << 0,
  MF_CASTLE   = 1 << 1,
  MF_DBLPAWN  = 1 << 2,
  MF_PROMO    = 1 << 3
};

inline constexpr Move make_move(int from, int to, Piece p, Piece cap, Piece promo, uint8_t flags) {
  return (Move)(from & 63)
      | ((Move)(to & 63) << 6)
      | ((Move)(p & 7) << 12)
      | ((Move)(cap & 7) << 15)
      | ((Move)(promo & 7) << 18)
      | ((Move)flags << 21);
}

inline constexpr int     m_from(Move m)   { return  m        & 63; }
inline constexpr int     m_to(Move m)     { return (m >>  6) & 63; }
inline constexpr Piece   m_piece(Move m)  { return (Piece)((m >> 12) & 7); }
inline constexpr Piece   m_cap(Move m)    { return (Piece)((m >> 15) & 7); }
inline constexpr Piece   m_promo(Move m)  { return (Piece)((m >> 18) & 7); }
inline constexpr uint8_t m_flags(Move m)  { return (uint8_t)((m >> 21) & 0xFF); }

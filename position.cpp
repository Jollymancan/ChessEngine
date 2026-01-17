#include "position.h"
#include "bitboard.h"
#include "attacks.h"

static constexpr int WK = 1, WQ = 2, BK = 4, BQ = 8;

Position::Position() {
  std::fill(std::begin(board), std::end(board), EMPTY_CODE);
}

void Position::rebuild_occ() {
  occ[WHITE] = occ[BLACK] = 0;
  for(int p=0;p<6;p++){
    occ[WHITE] |= bb[WHITE][p];
    occ[BLACK] |= bb[BLACK][p];
  }
  occAll = occ[WHITE] | occ[BLACK];
}

bool Position::is_attacked(int sq, Color by) const {
  // pawn (reverse lookup)
  if (by == WHITE) {
    if (ATK.pawn[BLACK][sq] & bb[WHITE][PAWN]) return true;
  } else {
    if (ATK.pawn[WHITE][sq] & bb[BLACK][PAWN]) return true;
  }

  if (ATK.knight[sq] & bb[by][KNIGHT]) return true;
  if (ATK.king[sq]   & bb[by][KING])   return true;

  U64 bq = bb[by][BISHOP] | bb[by][QUEEN];
  U64 rq = bb[by][ROOK]   | bb[by][QUEEN];

  if (bishop_attacks(sq, occAll) & bq) return true;
  if (rook_attacks(sq, occAll)   & rq) return true;

  return false;
}

void Position::gen_pseudo(MoveList& ml) const {
  ml.size = 0;
  const Color us = stm;
  const Color them = !stm;
  const U64 usOcc = occ[us];
  const U64 themOcc = occ[them];

  // pawns
  U64 pawns = bb[us][PAWN];
  while (pawns) {
    int from = pop_lsb(pawns);
    int r = rank_of(from);
    int dir = (us==WHITE) ? 8 : -8;
    int startRank = (us==WHITE) ? 1 : 6;
    int promoRank = (us==WHITE) ? 6 : 1;
    int to1 = from + dir;

    if (to1 >= 0 && to1 < 64 && board[to1] == EMPTY_CODE) {
      if (r == promoRank) {
        ml.push(make_move(from, to1, PAWN, NO_PIECE, QUEEN,  MF_PROMO));
        ml.push(make_move(from, to1, PAWN, NO_PIECE, ROOK,   MF_PROMO));
        ml.push(make_move(from, to1, PAWN, NO_PIECE, BISHOP, MF_PROMO));
        ml.push(make_move(from, to1, PAWN, NO_PIECE, KNIGHT, MF_PROMO));
      } else {
        ml.push(make_move(from, to1, PAWN, NO_PIECE, NO_PIECE, MF_NONE));
        if (r == startRank) {
          int to2 = from + 2*dir;
          if (board[to2] == EMPTY_CODE) {
            ml.push(make_move(from, to2, PAWN, NO_PIECE, NO_PIECE, MF_DBLPAWN));
          }
        }
      }
    }

    U64 caps = ATK.pawn[us][from] & themOcc;
    while (caps) {
      int to = pop_lsb(caps);
      Piece capP = code_piece(board[to]);
      if (r == promoRank) {
        ml.push(make_move(from, to, PAWN, capP, QUEEN,  MF_PROMO));
        ml.push(make_move(from, to, PAWN, capP, ROOK,   MF_PROMO));
        ml.push(make_move(from, to, PAWN, capP, BISHOP, MF_PROMO));
        ml.push(make_move(from, to, PAWN, capP, KNIGHT, MF_PROMO));
      } else {
        ml.push(make_move(from, to, PAWN, capP, NO_PIECE, MF_NONE));
      }
    }

    if (epSq != NO_SQ) {
      if (ATK.pawn[us][from] & sq_bb(epSq)) {
        ml.push(make_move(from, epSq, PAWN, PAWN, NO_PIECE, MF_EP));
      }
    }
  }

  // knights
  U64 knights = bb[us][KNIGHT];
  while (knights) {
    int from = pop_lsb(knights);
    U64 moves = ATK.knight[from] & ~usOcc;
    while (moves) {
      int to = pop_lsb(moves);
      if (board[to] == EMPTY_CODE) ml.push(make_move(from,to,KNIGHT,NO_PIECE,NO_PIECE,MF_NONE));
      else ml.push(make_move(from,to,KNIGHT,code_piece(board[to]),NO_PIECE,MF_NONE));
    }
  }

  // bishops
  U64 bishops = bb[us][BISHOP];
  while (bishops) {
    int from = pop_lsb(bishops);
    U64 moves = bishop_attacks(from, occAll) & ~usOcc;
    while (moves) {
      int to = pop_lsb(moves);
      if (board[to] == EMPTY_CODE) ml.push(make_move(from,to,BISHOP,NO_PIECE,NO_PIECE,MF_NONE));
      else ml.push(make_move(from,to,BISHOP,code_piece(board[to]),NO_PIECE,MF_NONE));
    }
  }

  // rooks
  U64 rooks = bb[us][ROOK];
  while (rooks) {
    int from = pop_lsb(rooks);
    U64 moves = rook_attacks(from, occAll) & ~usOcc;
    while (moves) {
      int to = pop_lsb(moves);
      if (board[to] == EMPTY_CODE) ml.push(make_move(from,to,ROOK,NO_PIECE,NO_PIECE,MF_NONE));
      else ml.push(make_move(from,to,ROOK,code_piece(board[to]),NO_PIECE,MF_NONE));
    }
  }

  // queens
  U64 queens = bb[us][QUEEN];
  while (queens) {
    int from = pop_lsb(queens);
    U64 moves = (rook_attacks(from, occAll) | bishop_attacks(from, occAll)) & ~usOcc;
    while (moves) {
      int to = pop_lsb(moves);
      if (board[to] == EMPTY_CODE) ml.push(make_move(from,to,QUEEN,NO_PIECE,NO_PIECE,MF_NONE));
      else ml.push(make_move(from,to,QUEEN,code_piece(board[to]),NO_PIECE,MF_NONE));
    }
  }

  // king
  {
    int from = kingSq[us];
    U64 moves = ATK.king[from] & ~usOcc;
    while (moves) {
      int to = pop_lsb(moves);
      if (board[to] == EMPTY_CODE) ml.push(make_move(from,to,KING,NO_PIECE,NO_PIECE,MF_NONE));
      else ml.push(make_move(from,to,KING,code_piece(board[to]),NO_PIECE,MF_NONE));
    }

    // castling (needs through-squares not attacked)
    if (us == WHITE) {
      if ((castling & WK) && board[5]==EMPTY_CODE && board[6]==EMPTY_CODE) {
        if (!is_attacked(4, them) && !is_attacked(5, them) && !is_attacked(6, them))
          ml.push(make_move(4,6,KING,NO_PIECE,NO_PIECE,MF_CASTLE));
      }
      if ((castling & WQ) && board[3]==EMPTY_CODE && board[2]==EMPTY_CODE && board[1]==EMPTY_CODE) {
        if (!is_attacked(4, them) && !is_attacked(3, them) && !is_attacked(2, them))
          ml.push(make_move(4,2,KING,NO_PIECE,NO_PIECE,MF_CASTLE));
      }
    } else {
      if ((castling & BK) && board[61]==EMPTY_CODE && board[62]==EMPTY_CODE) {
        if (!is_attacked(60, them) && !is_attacked(61, them) && !is_attacked(62, them))
          ml.push(make_move(60,62,KING,NO_PIECE,NO_PIECE,MF_CASTLE));
      }
      if ((castling & BQ) && board[59]==EMPTY_CODE && board[58]==EMPTY_CODE && board[57]==EMPTY_CODE) {
        if (!is_attacked(60, them) && !is_attacked(59, them) && !is_attacked(58, them))
          ml.push(make_move(60,58,KING,NO_PIECE,NO_PIECE,MF_CASTLE));
      }
    }
  }
}

void Position::make(Move m, Undo& u) {
  u.castling = castling;
  u.epSq = epSq;
  u.capturedCode = EMPTY_CODE;

  const int from = m_from(m);
  const int to   = m_to(m);
  const Piece p  = m_piece(m);
  const Piece cap= m_cap(m);
  const Piece promo = m_promo(m);
  const uint8_t flags = m_flags(m);

  Color us = stm;
  Color them = !stm;

  epSq = NO_SQ;

  if (cap != NO_PIECE && !(flags & MF_EP)) {
    u.capturedCode = board[to];
    bb[them][cap] ^= sq_bb(to);
    board[to] = EMPTY_CODE;
  }

  bb[us][p] ^= sq_bb(from);
  board[from] = EMPTY_CODE;

  if (flags & MF_EP) {
    int capSq = (us == WHITE) ? (to - 8) : (to + 8);
    u.capturedCode = board[capSq];
    bb[them][PAWN] ^= sq_bb(capSq);
    board[capSq] = EMPTY_CODE;
  }

  if (flags & MF_CASTLE) {
    if (us == WHITE) {
      if (to == 6) { // h1->f1
        bb[WHITE][ROOK] ^= sq_bb(7); bb[WHITE][ROOK] ^= sq_bb(5);
        board[7] = EMPTY_CODE; board[5] = code(WHITE, ROOK);
      } else { // a1->d1
        bb[WHITE][ROOK] ^= sq_bb(0); bb[WHITE][ROOK] ^= sq_bb(3);
        board[0] = EMPTY_CODE; board[3] = code(WHITE, ROOK);
      }
    } else {
      if (to == 62) { // h8->f8
        bb[BLACK][ROOK] ^= sq_bb(63); bb[BLACK][ROOK] ^= sq_bb(61);
        board[63] = EMPTY_CODE; board[61] = code(BLACK, ROOK);
      } else { // a8->d8
        bb[BLACK][ROOK] ^= sq_bb(56); bb[BLACK][ROOK] ^= sq_bb(59);
        board[56] = EMPTY_CODE; board[59] = code(BLACK, ROOK);
      }
    }
  }

  if (flags & MF_PROMO) {
    bb[us][promo] ^= sq_bb(to);
    board[to] = code(us, promo);
  } else {
    bb[us][p] ^= sq_bb(to);
    board[to] = code(us, p);
  }

  if (p == KING) {
    kingSq[us] = to;
    castling &= (us == WHITE) ? uint8_t(~(WK|WQ)) : uint8_t(~(BK|BQ));
  }

  if (p == ROOK) {
    if (from == 0) castling &= ~WQ;
    if (from == 7) castling &= ~WK;
    if (from == 56) castling &= ~BQ;
    if (from == 63) castling &= ~BK;
  }

  if (flags & MF_DBLPAWN) {
    epSq = (us == WHITE) ? (from + 8) : (from - 8);
  }

  stm = them;
  rebuild_occ();
}

void Position::unmake(Move m, const Undo& u) {
  const int from = m_from(m);
  const int to   = m_to(m);
  const Piece p  = m_piece(m);
  const Piece cap= m_cap(m);
  const Piece promo = m_promo(m);
  const uint8_t flags = m_flags(m);

  stm = !stm;
  Color us = stm;
  Color them = !stm;

  castling = u.castling;
  epSq = u.epSq;

  if (flags & MF_PROMO) {
    bb[us][promo] ^= sq_bb(to);
    board[to] = EMPTY_CODE;
    bb[us][PAWN] ^= sq_bb(from);
    board[from] = code(us, PAWN);
  } else {
    bb[us][p] ^= sq_bb(to);
    board[to] = EMPTY_CODE;
    bb[us][p] ^= sq_bb(from);
    board[from] = code(us, p);
  }

  if (flags & MF_CASTLE) {
    if (us == WHITE) {
      if (to == 6) {
        bb[WHITE][ROOK] ^= sq_bb(5); bb[WHITE][ROOK] ^= sq_bb(7);
        board[5] = EMPTY_CODE; board[7] = code(WHITE, ROOK);
      } else {
        bb[WHITE][ROOK] ^= sq_bb(3); bb[WHITE][ROOK] ^= sq_bb(0);
        board[3] = EMPTY_CODE; board[0] = code(WHITE, ROOK);
      }
    } else {
      if (to == 62) {
        bb[BLACK][ROOK] ^= sq_bb(61); bb[BLACK][ROOK] ^= sq_bb(63);
        board[61] = EMPTY_CODE; board[63] = code(BLACK, ROOK);
      } else {
        bb[BLACK][ROOK] ^= sq_bb(59); bb[BLACK][ROOK] ^= sq_bb(56);
        board[59] = EMPTY_CODE; board[56] = code(BLACK, ROOK);
      }
    }
  }

  if (u.capturedCode != EMPTY_CODE) {
    if (flags & MF_EP) {
      int capSq = (us == WHITE) ? (to - 8) : (to + 8);
      board[capSq] = u.capturedCode;
      bb[them][PAWN] ^= sq_bb(capSq);
    } else {
      board[to] = u.capturedCode;
      Piece cp = code_piece(u.capturedCode);
      bb[them][cp] ^= sq_bb(to);
    }
  }

  if (p == KING) kingSq[us] = from;

  (void)cap;
  rebuild_occ();
}

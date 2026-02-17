#include "position.h"
#include "bitboard.h"
#include "attacks.h"
#include "zobrist.h"

static constexpr int WK = 1, WQ = 2, BK = 4, BQ = 8;

Position::Position() {
  std::fill(std::begin(board), std::end(board), EMPTY_CODE);
  halfmoveClock = 0;
  fullmoveNumber = 1;
  gameKeys.clear();
}

void Position::rebuild_occ() {
  occ[WHITE] = occ[BLACK] = 0;
  for(int p=0;p<6;p++){
    occ[WHITE] |= bb[WHITE][p];
    occ[BLACK] |= bb[BLACK][p];
  }
  occAll = occ[WHITE] | occ[BLACK];
}

static inline int ep_file_or_none(int epSq) {
  return (epSq == NO_SQ) ? 8 : (epSq & 7);
}

void Position::rebuild_pawn_key() {
  uint64_t pk = 0;
  U64 wp = bb[WHITE][PAWN];
  U64 bp = bb[BLACK][PAWN];
  while (wp) {
    int sq = pop_lsb(wp);
    pk ^= ZP[code(WHITE, PAWN)][sq];
  }
  while (bp) {
    int sq = pop_lsb(bp);
    pk ^= ZP[code(BLACK, PAWN)][sq];
  }
  pawnKey = pk;
}

void Position::rebuild_key() {
  uint64_t k = 0;
  for (int sq=0; sq<64; sq++) {
    int c = board[sq];
    if (c != EMPTY_CODE) k ^= ZP[c][sq];
  }
  if (stm == BLACK) k ^= ZSide;
  k ^= ZCastle[castling & 15];
  k ^= ZEP[ep_file_or_none(epSq)];
  key = k;
  rebuild_pawn_key();
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
  u.key = key;
  u.pawnKey = pawnKey;
  u.occ[0] = occ[0];
  u.occ[1] = occ[1];
  u.halfmoveClock = halfmoveClock;
  u.fullmoveNumber = fullmoveNumber;
  u.halfmoveClock = halfmoveClock;
  u.fullmoveNumber = fullmoveNumber;

  const int from = m_from(m);
  const int to   = m_to(m);
  const Piece p  = m_piece(m);
  const Piece cap= m_cap(m);
  const Piece promo = m_promo(m);
  const uint8_t flags = m_flags(m);

  Color us = stm;
  Color them = !stm;

  // remove state components that will change
  key ^= ZCastle[castling & 15];
  key ^= ZEP[ep_file_or_none(epSq)];

  epSq = NO_SQ;

  if (cap != NO_PIECE && !(flags & MF_EP)) {
    u.capturedCode = board[to];
    bb[them][cap] ^= sq_bb(to);
    board[to] = EMPTY_CODE;

    // zobrist + occupancy
    key ^= ZP[u.capturedCode][to];
    if (cap == PAWN) pawnKey ^= ZP[u.capturedCode][to];
    occ[them] ^= sq_bb(to);

    // If a rook was captured on its home square, clear the corresponding castling right.
    if (cap == ROOK) {
      if (to == 0)  castling &= ~WQ;
      if (to == 7)  castling &= ~WK;
      if (to == 56) castling &= ~BQ;
      if (to == 63) castling &= ~BK;
    }
  }

  bb[us][p] ^= sq_bb(from);
  board[from] = EMPTY_CODE;

  key ^= ZP[code(us,p)][from];
  if (p == PAWN) pawnKey ^= ZP[code(us,PAWN)][from];
  occ[us] ^= sq_bb(from);

  if (flags & MF_EP) {
    int capSq = (us == WHITE) ? (to - 8) : (to + 8);
    u.capturedCode = board[capSq];
    bb[them][PAWN] ^= sq_bb(capSq);
    board[capSq] = EMPTY_CODE;

    key ^= ZP[u.capturedCode][capSq];
    pawnKey ^= ZP[u.capturedCode][capSq];
    occ[them] ^= sq_bb(capSq);
  }

  if (flags & MF_CASTLE) {
    if (us == WHITE) {
      if (to == 6) { // h1->f1
        bb[WHITE][ROOK] ^= sq_bb(7); bb[WHITE][ROOK] ^= sq_bb(5);
        board[7] = EMPTY_CODE; board[5] = code(WHITE, ROOK);

        key ^= ZP[code(WHITE,ROOK)][7] ^ ZP[code(WHITE,ROOK)][5];
        occ[WHITE] ^= sq_bb(7) ^ sq_bb(5);
      } else { // a1->d1
        bb[WHITE][ROOK] ^= sq_bb(0); bb[WHITE][ROOK] ^= sq_bb(3);
        board[0] = EMPTY_CODE; board[3] = code(WHITE, ROOK);

        key ^= ZP[code(WHITE,ROOK)][0] ^ ZP[code(WHITE,ROOK)][3];
        occ[WHITE] ^= sq_bb(0) ^ sq_bb(3);
      }
    } else {
      if (to == 62) { // h8->f8
        bb[BLACK][ROOK] ^= sq_bb(63); bb[BLACK][ROOK] ^= sq_bb(61);
        board[63] = EMPTY_CODE; board[61] = code(BLACK, ROOK);

        key ^= ZP[code(BLACK,ROOK)][63] ^ ZP[code(BLACK,ROOK)][61];
        occ[BLACK] ^= sq_bb(63) ^ sq_bb(61);
      } else { // a8->d8
        bb[BLACK][ROOK] ^= sq_bb(56); bb[BLACK][ROOK] ^= sq_bb(59);
        board[56] = EMPTY_CODE; board[59] = code(BLACK, ROOK);

        key ^= ZP[code(BLACK,ROOK)][56] ^ ZP[code(BLACK,ROOK)][59];
        occ[BLACK] ^= sq_bb(56) ^ sq_bb(59);
      }
    }
  }

  if (flags & MF_PROMO) {
    bb[us][promo] ^= sq_bb(to);
    board[to] = code(us, promo);

    key ^= ZP[code(us,promo)][to];
    occ[us] ^= sq_bb(to);
  } else {
    bb[us][p] ^= sq_bb(to);
    board[to] = code(us, p);

    key ^= ZP[code(us,p)][to];
    if (p == PAWN) pawnKey ^= ZP[code(us,PAWN)][to];
    occ[us] ^= sq_bb(to);
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

  // add back updated state components
  key ^= ZCastle[castling & 15];
  key ^= ZEP[ep_file_or_none(epSq)];

  // 50-move counter
  bool reset50 = (p == PAWN) || (cap != NO_PIECE) || (flags & MF_EP) || (flags & MF_PROMO);
  if (reset50) halfmoveClock = 0;
  else halfmoveClock = (uint16_t)std::min<int>(halfmoveClock + 1, 1000);

  // flip side-to-move
  stm = them;
  key ^= ZSide;

  // Fullmove increments after Black makes a move.
  if (stm == WHITE) fullmoveNumber++;

  occAll = occ[WHITE] | occ[BLACK];
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
  key = u.key;
  pawnKey = u.pawnKey;
  occ[0] = u.occ[0];
  occ[1] = u.occ[1];
  halfmoveClock = u.halfmoveClock;
  fullmoveNumber = u.fullmoveNumber;

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
  occAll = occ[WHITE] | occ[BLACK];
}

void Position::make_null(Undo& u) {
  u.castling = castling;
  u.epSq = epSq;
  u.capturedCode = EMPTY_CODE;
  u.key = key;
  u.pawnKey = pawnKey;
  u.occ[0] = occ[0];
  u.occ[1] = occ[1];
  u.halfmoveClock = halfmoveClock;
  u.fullmoveNumber = fullmoveNumber;

  // remove old ep component
  key ^= ZEP[ep_file_or_none(epSq)];
  epSq = NO_SQ;
  key ^= ZEP[8];

  // null move counts as a halfmove for 50-move clock
  halfmoveClock = (uint16_t)std::min<int>(halfmoveClock + 1, 1000);

  // flip side
  stm = !stm;
  key ^= ZSide;

  if (stm == WHITE) fullmoveNumber++;
}

void Position::unmake_null(const Undo& u) {
  castling = u.castling;
  epSq = u.epSq;
  key = u.key;
  pawnKey = u.pawnKey;
  occ[0] = u.occ[0];
  occ[1] = u.occ[1];
  halfmoveClock = u.halfmoveClock;
  fullmoveNumber = u.fullmoveNumber;
  stm = !stm;
}


void Position::reset_game_history() {
  gameKeys.clear();
  gameKeys.push_back(key);
}

void Position::push_game_key() {
  gameKeys.push_back(key);
}

int Position::repetition_count() const {
  int cnt = 0;
  for (U64 k : gameKeys) if (k == key) cnt++;
  return cnt;
}

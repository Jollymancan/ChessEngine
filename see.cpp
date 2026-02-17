#include "see.h"
#include "attacks.h"
#include "bitboard.h"

// Simple piece values for SEE (centipawns)
static inline int see_val(Piece p) {
  static constexpr int v[6] = {100, 320, 330, 500, 900, 20000};
  return v[(int)p];
}

static inline U64 attackers_to(int sq, U64 occ, const U64 pieces[2][6], Color c) {
  U64 att = 0;
  // pawns (reverse attack)
  att |= (ATK.pawn[!c][sq] & pieces[c][PAWN]);
  att |= (ATK.knight[sq] & pieces[c][KNIGHT]);
  att |= (ATK.king[sq] & pieces[c][KING]);

  U64 bishops = pieces[c][BISHOP] | pieces[c][QUEEN];
  U64 rooks   = pieces[c][ROOK]   | pieces[c][QUEEN];

  att |= (bishop_attacks(sq, occ) & bishops);
  att |= (rook_attacks(sq, occ)   & rooks);
  return att;
}

static inline int pop_least_valuable_attacker(U64& att, U64 pieces[2][6], Color c, Piece& outPiece) {
  // order: pawn, knight, bishop, rook, queen, king
  for (int p=PAWN; p<=KING; p++) {
    U64 bb = att & pieces[c][p];
    if (bb) {
      int from = pop_lsb(bb);
      U64 sqbb = sq_bb(from);
      // remove this attacker from sets
      att ^= sqbb;
      pieces[c][p] ^= sqbb;
      outPiece = (Piece)p;
      return from;
    }
  }
  return -1;
}

int see(const Position& pos, Move m) {
  const uint8_t flags = m_flags(m);
  const int from = m_from(m);
  const int to   = m_to(m);
  const Color us = pos.stm;
  const Color them = !us;

  // Only meaningful for captures / ep / promotions (we use it for pruning)
  Piece capP = m_cap(m);
  int capSq = to;
  if (flags & MF_EP) { capP = PAWN; capSq = (us == WHITE) ? (to - 8) : (to + 8); }

  if (capP == NO_PIECE && !(flags & MF_PROMO)) return 0;

  // Local copies of piece bitboards (we'll mutate)
  U64 pieces[2][6];
  for (int c=0;c<2;c++) for (int p=0;p<6;p++) pieces[c][p] = pos.bb[c][p];

  U64 occ = pos.occAll;

  Piece moving = m_piece(m);
  Piece pieceOnSq = moving;
  if (flags & MF_PROMO) {
    // after move, pawn becomes promo piece
    pieceOnSq = m_promo(m);
  }

  // apply initial move to local sets
  const U64 fromBB = sq_bb(from);
  const U64 toBB   = sq_bb(to);
  pieces[us][moving] ^= fromBB;
  pieces[us][pieceOnSq] |= toBB;
  occ ^= fromBB;
  occ |= toBB;

  // remove captured piece
  if (capP != NO_PIECE) {
    const U64 capBB = sq_bb(capSq);
    pieces[them][capP] ^= capBB;
    occ ^= capBB;
  }

  // If it was a promotion, remove the pawn already (we did by removing moving piece at from).
  // If promo piece == moving piece (shouldn't), ok.

  int gain[32];
  int d = 0;
  gain[d] = see_val(capP == NO_PIECE ? PAWN : capP);

  // attackers to 'to' with updated occupancy
  U64 attW = attackers_to(to, occ, pieces, WHITE);
  U64 attB = attackers_to(to, occ, pieces, BLACK);

  // Side to recapture first
  Color side = them;

  Piece victim = pieceOnSq;

  while (true) {
    U64& att = (side == WHITE) ? attW : attB;
    // remove any attackers that are not actually present anymore (safety)
    att &= (pieces[side][PAWN] | pieces[side][KNIGHT] | pieces[side][BISHOP] |
            pieces[side][ROOK] | pieces[side][QUEEN]  | pieces[side][KING]);
    if (!att) break;

    Piece aPiece = PAWN;
    int aFrom = pop_least_valuable_attacker(att, pieces, side, aPiece);
    if (aFrom < 0) break;

    d++;
    gain[d] = see_val(victim) - gain[d-1];

    // move attacker onto square: remove from occ, add to square (conceptually on sq)
    const U64 aBB = sq_bb(aFrom);
    occ ^= aBB;
    // attacker now occupies 'to' (we don't need to set in pieces[side] because it can be captured next;
    // we track the "victim" piece type only, and attackers are recomputed from occ + pieces arrays.)

    // Update victim to attacker piece
    victim = aPiece;

    // Recompute sliding attacks (x-rays) after removing attacker from occ
    attW = attackers_to(to, occ, pieces, WHITE);
    attB = attackers_to(to, occ, pieces, BLACK);

    side = !side;
    if (d >= 30) break;
  }

  // minimax backward
  for (int i=d-1;i>=0;i--) gain[i] = std::max(gain[i], -gain[i+1]);
  return gain[0];
}

bool see_ge(const Position& pos, Move m, int threshold) {
  return see(pos, m) >= threshold;
}

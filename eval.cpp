#include "eval.h"
#include "params.h"
#include "attacks.h"
#include "bitboard.h"
#include <algorithm>
#include <cstdint>

// Queen = rook | bishop
static inline U64 queen_attacks(int sq, U64 occ) {
  return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

static inline int mirror_sq(int sq) { return sq ^ 56; } // flip ranks for black PST indexing

// ------------------------------------------------------------
// Tunable weights
// ------------------------------------------------------------
static constexpr int TEMPO_BONUS = 10;

// Bishop pair
static constexpr int BISHOP_PAIR_BONUS_MG = 30;
static constexpr int BISHOP_PAIR_BONUS_EG = 40;

// Pawn structure
static constexpr int DOUBLED_PAWN_PEN_MG = 12;
static constexpr int DOUBLED_PAWN_PEN_EG = 8;

static constexpr int ISOLATED_PAWN_PEN_MG = 14;
static constexpr int ISOLATED_PAWN_PEN_EG = 10;

static constexpr int CONNECTED_PASSED_BONUS_MG = 10;
static constexpr int CONNECTED_PASSED_BONUS_EG = 18;

// Rooks on files
static constexpr int ROOK_OPEN_FILE_BONUS_MG = 18;
static constexpr int ROOK_OPEN_FILE_BONUS_EG = 10;
static constexpr int ROOK_SEMIOPEN_FILE_BONUS_MG = 10;
static constexpr int ROOK_SEMIOPEN_FILE_BONUS_EG = 6;

// Extra rook features
static constexpr int ROOK_7TH_BONUS_MG = 20;
static constexpr int ROOK_7TH_BONUS_EG = 25;
static constexpr int CONNECTED_ROOKS_BONUS_MG = 10;
static constexpr int CONNECTED_ROOKS_BONUS_EG = 16;

// Mobility weights (per attacked square)
static constexpr int MOB_N_MG = 4,  MOB_N_EG = 4;
static constexpr int MOB_B_MG = 4,  MOB_B_EG = 5;
static constexpr int MOB_R_MG = 2,  MOB_R_EG = 3;
static constexpr int MOB_Q_MG = 1,  MOB_Q_EG = 2;

// King safety
static constexpr int KING_SHIELD_BONUS = 8;    // per pawn in front-of-king shield squares (MG)
static constexpr int KING_OPEN_FILE_PEN = 15;  // per open file on/adjacent to king file (MG)
static constexpr int KING_RING_ATTACK_W = 6;   // per attacked king-ring square (MG)
static constexpr int KING_PRESSURE_BONUS = 10; // if ring attacks >= threshold (MG)
static constexpr int KING_PRESSURE_TH = 6;

// Threats / hanging pieces
static constexpr int HANG_P_MG = 8,  HANG_P_EG = 6;
static constexpr int HANG_N_MG = 18, HANG_N_EG = 14;
static constexpr int HANG_B_MG = 18, HANG_B_EG = 14;
static constexpr int HANG_R_MG = 28, HANG_R_EG = 22;
static constexpr int HANG_Q_MG = 40, HANG_Q_EG = 32;

// Outposts + bishop quality
static constexpr int OUTPOST_N_MG = 18;
static constexpr int OUTPOST_N_EG = 10;
static constexpr int BAD_BISHOP_PEN_MG = 8; // per same-color pawn
static constexpr int BAD_BISHOP_PEN_EG = 4;

// Passed pawn bonus by rank (rank from your side: 0..7)
static constexpr int PASSED_MG[8] = {0,  5, 10, 20, 35, 55, 85, 0};
static constexpr int PASSED_EG[8] = {0, 10, 20, 35, 55, 85,120, 0};

// Tapered material values
static constexpr int MG_VAL[6] = { 82, 337, 365, 477, 1025, 0 };
static constexpr int EG_VAL[6] = { 94, 281, 297, 512,  936, 0 };

// Phase increments (how much each piece contributes to "middlegame-ness")
static constexpr int PHASE_INC[6] = {0, 1, 1, 2, 4, 0};
static constexpr int TOTAL_PHASE = 24;

// ------------------------------------------------------------
// PSTs
// ------------------------------------------------------------
static constexpr int PST_P_MG[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
  10, 12,  6, -5, -5,  6, 12, 10,
   4,  4,  2,  8,  8,  2,  4,  4,
   2,  2,  6, 14, 14,  6,  2,  2,
   2,  4,  8, 18, 18,  8,  4,  2,
   4,  6, 10,  0,  0, 10,  6,  4,
  40, 40, 40, 40, 40, 40, 40, 40,
   0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_P_EG[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
  20, 18, 16, 14, 14, 16, 18, 20,
  12, 12, 12, 12, 12, 12, 12, 12,
   8, 10, 12, 14, 14, 12, 10,  8,
   6,  8, 10, 12, 12, 10,  8,  6,
   4,  6,  8, 10, 10,  8,  6,  4,
   2,  2,  2,  2,  2,  2,  2,  2,
   0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_N_MG[64] = {
 -50,-40,-30,-30,-30,-30,-40,-50,
 -40,-20,  0,  0,  0,  0,-20,-40,
 -30,  0, 10, 15, 15, 10,  0,-30,
 -30,  5, 15, 20, 20, 15,  5,-30,
 -30,  0, 15, 20, 20, 15,  0,-30,
 -30,  5, 10, 15, 15, 10,  5,-30,
 -40,-20,  0,  5,  5,  0,-20,-40,
 -50,-40,-30,-30,-30,-30,-40,-50
};

static constexpr int PST_N_EG[64] = {
 -40,-30,-20,-20,-20,-20,-30,-40,
 -30,-10,  0,  0,  0,  0,-10,-30,
 -20,  0, 10, 12, 12, 10,  0,-20,
 -20,  5, 12, 18, 18, 12,  5,-20,
 -20,  0, 12, 18, 18, 12,  0,-20,
 -20,  5, 10, 12, 12, 10,  5,-20,
 -30,-10,  0,  5,  5,  0,-10,-30,
 -40,-30,-20,-20,-20,-20,-30,-40
};

static constexpr int PST_B_MG[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5, 10, 10,  5,  0,-10,
 -10,  5,  5, 10, 10,  5,  5,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -20,-10,-10,-10,-10,-10,-10,-20
};

static constexpr int PST_B_EG[64] = {
 -15,-10,-10,-10,-10,-10,-10,-15,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  8, 10, 10,  8,  0,-10,
 -10,  8, 10, 12, 12, 10,  8,-10,
 -10,  0, 10, 12, 12, 10,  0,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -15,-10,-10,-10,-10,-10,-10,-15
};

static constexpr int PST_R_MG[64] = {
   0,  0,  5, 10, 10,  5,  0,  0,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
   5, 10, 10, 10, 10, 10, 10,  5,
   0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_R_EG[64] = {
   0,  0,  5,  8,  8,  5,  0,  0,
   0,  0,  0,  2,  2,  0,  0,  0,
   0,  0,  0,  2,  2,  0,  0,  0,
   0,  0,  0,  2,  2,  0,  0,  0,
   0,  0,  0,  2,  2,  0,  0,  0,
   0,  0,  0,  2,  2,  0,  0,  0,
   5,  8,  8, 10, 10,  8,  8,  5,
   0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_Q_MG[64] = {
 -20,-10,-10, -5, -5,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0,  5,  5,  5,  5,  0,-10,
  -5,  0,  5,  5,  5,  5,  0, -5,
   0,  0,  5,  5,  5,  5,  0, -5,
 -10,  5,  5,  5,  5,  5,  0,-10,
 -10,  0,  5,  0,  0,  0,  0,-10,
 -20,-10,-10, -5, -5,-10,-10,-20
};

static constexpr int PST_Q_EG[64] = {
 -10, -5, -5, -2, -2, -5, -5,-10,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  5,  5,  5,  5,  0, -5,
  -2,  0,  5,  6,  6,  5,  0, -2,
  -2,  0,  5,  6,  6,  5,  0, -2,
  -5,  0,  5,  5,  5,  5,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
 -10, -5, -5, -2, -2, -5, -5,-10
};

static constexpr int PST_K_MG[64] = {
 -50,-40,-30,-20,-20,-30,-40,-50,
 -40,-30,-20,-10,-10,-20,-30,-40,
 -30,-20,-10,  0,  0,-10,-20,-30,
 -20,-10,  0, 10, 10,  0,-10,-20,
 -20,-10,  0, 10, 10,  0,-10,-20,
 -30,-20,-10,  0,  0,-10,-20,-30,
 -40,-30,-20,-10,-10,-20,-30,-40,
 -50,-40,-30,-20,-20,-30,-40,-50
};

static constexpr int PST_K_EG[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10,  0, 10, 20, 20, 10,  0,-10,
 -10,  0, 10, 20, 20, 10,  0,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -20,-10,-10,-10,-10,-10,-10,-20
};

static inline void pst_add(int piece, int sqW, int& mg, int& eg, int sign) {
  switch (piece) {
    case PAWN:   mg += sign * PST_P_MG[sqW]; eg += sign * PST_P_EG[sqW]; break;
    case KNIGHT: mg += sign * PST_N_MG[sqW]; eg += sign * PST_N_EG[sqW]; break;
    case BISHOP: mg += sign * PST_B_MG[sqW]; eg += sign * PST_B_EG[sqW]; break;
    case ROOK:   mg += sign * PST_R_MG[sqW]; eg += sign * PST_R_EG[sqW]; break;
    case QUEEN:  mg += sign * PST_Q_MG[sqW]; eg += sign * PST_Q_EG[sqW]; break;
    case KING:   mg += sign * PST_K_MG[sqW]; eg += sign * PST_K_EG[sqW]; break;
  }
}

// ------------------------------------------------------------
// Masks and helpers
// ------------------------------------------------------------
static U64 FILE_MASK[8];
static U64 ADJ_FILE_MASK[8];
static bool masks_inited = false;

static void init_masks_once() {
  if (masks_inited) return;
  masks_inited = true;

  for (int f=0; f<8; f++){
    U64 m = 0;
    for (int r=0; r<8; r++) m |= (1ULL << (r*8 + f));
    FILE_MASK[f] = m;

    U64 adj = 0;
    if (f > 0) adj |= FILE_MASK[f-1];
    if (f < 7) adj |= FILE_MASK[f+1];
    ADJ_FILE_MASK[f] = adj;
  }
}

static inline int pawn_rank_from_side(Color c, int sq) {
  int r = rank_of(sq);
  return (c == WHITE) ? r : (7 - r); // 0..7
}

static inline bool is_passed_pawn(Color c, int sq, U64 oppPawns) {
  int f = file_of(sq);
  int r = rank_of(sq);

  U64 front = 0;
  if (c == WHITE) {
    for (int rr = r+1; rr < 8; rr++){
      front |= sq_bb(rr*8 + f);
      if (f > 0) front |= sq_bb(rr*8 + (f-1));
      if (f < 7) front |= sq_bb(rr*8 + (f+1));
    }
  } else {
    for (int rr = r-1; rr >= 0; rr--){
      front |= sq_bb(rr*8 + f);
      if (f > 0) front |= sq_bb(rr*8 + (f-1));
      if (f < 7) front |= sq_bb(rr*8 + (f+1));
    }
  }
  return (oppPawns & front) == 0;
}

static inline U64 attacks_for_side(const Position& pos, Color c, U64 occAll) {
  U64 att = 0;

  U64 pawns = pos.bb[c][PAWN];
  while (pawns) { int sq = pop_lsb(pawns); att |= ATK.pawn[c][sq]; }

  U64 knights = pos.bb[c][KNIGHT];
  while (knights) { int sq = pop_lsb(knights); att |= ATK.knight[sq]; }

  U64 bishops = pos.bb[c][BISHOP];
  while (bishops) { int sq = pop_lsb(bishops); att |= bishop_attacks(sq, occAll); }

  U64 rooks = pos.bb[c][ROOK];
  while (rooks) { int sq = pop_lsb(rooks); att |= rook_attacks(sq, occAll); }

  U64 queens = pos.bb[c][QUEEN];
  while (queens) { int sq = pop_lsb(queens); att |= queen_attacks(sq, occAll); }

  att |= ATK.king[pos.kingSq[c]];

  return att;
}

static inline void king_safety_shield_openfiles(const Position& pos, Color c, U64 myP, U64 allP, int& mg, int sign) {
  int ksq = pos.kingSq[c];
  int kf = file_of(ksq);
  int kr = rank_of(ksq);

  // Pawn shield: 3 squares one rank in front of king (and diagonals)
  U64 shield = 0;
  if (c == WHITE) {
    int rr = kr + 1;
    if (rr < 8) {
      if (kf > 0) shield |= sq_bb(rr*8 + (kf-1));
      shield |= sq_bb(rr*8 + kf);
      if (kf < 7) shield |= sq_bb(rr*8 + (kf+1));
    }
  } else {
    int rr = kr - 1;
    if (rr >= 0) {
      if (kf > 0) shield |= sq_bb(rr*8 + (kf-1));
      shield |= sq_bb(rr*8 + kf);
      if (kf < 7) shield |= sq_bb(rr*8 + (kf+1));
    }
  }

  int shieldCount = popcount64(myP & shield);
  mg += sign * (shieldCount * KING_SHIELD_BONUS);

  // open file penalty on king file and adjacent files
  int openFiles = 0;
  for (int f = std::max(0, kf-1); f <= std::min(7, kf+1); f++) {
    if ((allP & FILE_MASK[f]) == 0) openFiles++;
  }
  mg -= sign * (openFiles * KING_OPEN_FILE_PEN);
}

static inline void hanging_penalties(const Position& pos, Color us, U64 attUs, U64 attThem, int& mg, int& eg) {
  int sign = (us == WHITE) ? +1 : -1;

  auto pen = [&](int p)->std::pair<int,int>{
    switch(p){
      case PAWN:   return {HANG_P_MG, HANG_P_EG};
      case KNIGHT: return {HANG_N_MG, HANG_N_EG};
      case BISHOP: return {HANG_B_MG, HANG_B_EG};
      case ROOK:   return {HANG_R_MG, HANG_R_EG};
      case QUEEN:  return {HANG_Q_MG, HANG_Q_EG};
      default:     return {0,0};
    }
  };

  for (int p=PAWN; p<=QUEEN; p++){
    U64 bb = pos.bb[us][p];
    while (bb){
      int sq = pop_lsb(bb);
      U64 sbb = sq_bb(sq);
      bool attacked = (attThem & sbb) != 0;
      bool defended = (attUs   & sbb) != 0;
      if (attacked && !defended) {
        auto [pm, pe] = pen(p);
        mg -= sign * pm;
        eg -= sign * pe;
      }
    }
  }
}

static constexpr U64 DARK_SQ  = 0xAA55AA55AA55AA55ULL; // a1 dark
static constexpr U64 LIGHT_SQ = ~DARK_SQ;

// ------------------------------------------------------------
// Pawn hash (caches pawn-structure evaluation)
// ------------------------------------------------------------
namespace {
  static constexpr size_t PAWN_TT_SIZE = 1u << 18; // 262k
  struct PawnEntry {
    uint64_t key = 0;
    int mg = 0;
    int eg = 0;
    U64 passedW = 0;
    U64 passedB = 0;
    U64 connectedW = 0;
    U64 connectedB = 0;
  };
  static PawnEntry PawnTT[PAWN_TT_SIZE];


    static inline uint64_t pawn_key(const Position& pos) {
    return pos.pawnKey;
  }
}

static inline bool supported_by_pawn(Color c, int sq, U64 pawns) {
  // squares that attack sq with a pawn of color c
  // white pawn attackers to sq are ATK.pawn[BLACK][sq]
  U64 attackers = (c == WHITE) ? ATK.pawn[BLACK][sq] : ATK.pawn[WHITE][sq];
  return (pawns & attackers) != 0;
}

static inline bool enemy_pawn_can_chase(Color us, int sq, U64 enemyPawns) {
  int f = file_of(sq);
  int r = rank_of(sq);

  if (us == WHITE) {
    // black pawns move down; they come from higher ranks
    for (int ff = std::max(0, f-1); ff <= std::min(7, f+1); ff++) {
      if (ff == f) continue;
      for (int rr = r+1; rr < 8; rr++) {
        if (enemyPawns & sq_bb(rr*8 + ff)) return true;
      }
    }
  } else {
    // white pawns move up; they come from lower ranks
    for (int ff = std::max(0, f-1); ff <= std::min(7, f+1); ff++) {
      if (ff == f) continue;
      for (int rr = r-1; rr >= 0; rr--) {
        if (enemyPawns & sq_bb(rr*8 + ff)) return true;
      }
    }
  }
  return false;
}

// ------------------------------------------------------------
// eval()
// ------------------------------------------------------------
static int eval_uncached(const Position& pos) {
  init_masks_once();

  int mg = 0, eg = 0;
  int phase = 0;

  // Compute occupancy locally (no dependency on Position having occ fields)
  U64 occW = 0, occB = 0;
  for (int p=0; p<6; p++){
    occW |= pos.bb[WHITE][p];
    occB |= pos.bb[BLACK][p];
  }
  U64 occAll = occW | occB;

  // Material + PST + phase
  for (int c=0;c<2;c++){
    int sign = (c==WHITE) ? +1 : -1;

    for (int p=0;p<6;p++){
      U64 bb = pos.bb[c][p];
      int cnt = popcount64(bb);

      mg += sign * (MG_VAL[p] * cnt);
      eg += sign * (EG_VAL[p] * cnt);
      phase += PHASE_INC[p] * cnt;

      while (bb){
        int sq = pop_lsb(bb);
        int sqW = (c==WHITE) ? sq : mirror_sq(sq);
        pst_add(p, sqW, mg, eg, sign);
      }
    }
  }
  phase = std::min(phase, TOTAL_PHASE);

  // Bishop pair
  if (popcount64(pos.bb[WHITE][BISHOP]) >= 2) { mg += BISHOP_PAIR_BONUS_MG; eg += BISHOP_PAIR_BONUS_EG; }
  if (popcount64(pos.bb[BLACK][BISHOP]) >= 2) { mg -= BISHOP_PAIR_BONUS_MG; eg -= BISHOP_PAIR_BONUS_EG; }

  // Pawn structure: doubled / isolated / passed / connected passed (cached)
  const uint64_t pk = pawn_key(pos);
  PawnEntry& pe = PawnTT[pk & (PAWN_TT_SIZE - 1)];
  if (pe.key == pk) {
    mg += pe.mg;
    eg += pe.eg;
  } else {
    int pmg = 0, peg = 0;
    U64 passedW = 0, passedB = 0;
    U64 connW = 0, connB = 0;

    for (int c=0;c<2;c++){
      Color us = (Color)c;
      int sign = (us==WHITE) ? +1 : -1;

      U64 myP = pos.bb[us][PAWN];
      U64 oppP = pos.bb[us^1][PAWN];

      // doubled + isolated (file-based)
      for (int f=0; f<8; f++){
        int n = popcount64(myP & FILE_MASK[f]);
        if (n >= 2) {
          int extra = n - 1;
          pmg -= sign * (extra * DOUBLED_PAWN_PEN_MG);
          peg -= sign * (extra * DOUBLED_PAWN_PEN_EG);
        }
        if (n >= 1 && (myP & ADJ_FILE_MASK[f]) == 0) {
          pmg -= sign * (n * ISOLATED_PAWN_PEN_MG);
          peg -= sign * (n * ISOLATED_PAWN_PEN_EG);
        }
      }

      // passed
      U64 passedMask = 0;
      U64 pawns = myP;
      while (pawns){
        int sq = pop_lsb(pawns);
        if (is_passed_pawn(us, sq, oppP)) {
          passedMask |= sq_bb(sq);
          int pr = pawn_rank_from_side(us, sq);
          pmg += sign * PASSED_MG[pr];
          peg += sign * PASSED_EG[pr];
        }
      }

      // connected passers (cache a mask too)
      U64 connMask = 0;
      for (int f=0; f<8; f++){
        if ((passedMask & FILE_MASK[f]) == 0) continue;
        bool adj = false;
        if (f > 0 && (passedMask & FILE_MASK[f-1])) adj = true;
        if (f < 7 && (passedMask & FILE_MASK[f+1])) adj = true;
        if (adj) {
          connMask |= (passedMask & FILE_MASK[f]);
          pmg += sign * CONNECTED_PASSED_BONUS_MG;
          peg += sign * CONNECTED_PASSED_BONUS_EG;
        }
      }

      if (us == WHITE) { passedW = passedMask; connW = connMask; }
      else { passedB = passedMask; connB = connMask; }
    }

    pe.key = pk;
    pe.mg = pmg;
    pe.eg = peg;
    pe.passedW = passedW;
    pe.passedB = passedB;
    pe.connectedW = connW;
    pe.connectedB = connB;
    mg += pmg;
    eg += peg;
  }

  // Rooks on open/semi-open files
  for (int c=0;c<2;c++){
    Color us = (Color)c;
    int sign = (us==WHITE) ? +1 : -1;

    U64 myR = pos.bb[us][ROOK];
    U64 myP = pos.bb[us][PAWN];
    U64 oppP = pos.bb[us^1][PAWN];

    U64 rooks = myR;
    while (rooks){
      int sq = pop_lsb(rooks);
      int f = file_of(sq);

      bool myPawnOnFile = (myP & FILE_MASK[f]) != 0;
      bool oppPawnOnFile = (oppP & FILE_MASK[f]) != 0;

      if (!myPawnOnFile && !oppPawnOnFile) {
        mg += sign * ROOK_OPEN_FILE_BONUS_MG;
        eg += sign * ROOK_OPEN_FILE_BONUS_EG;
      } else if (!myPawnOnFile && oppPawnOnFile) {
        mg += sign * ROOK_SEMIOPEN_FILE_BONUS_MG;
        eg += sign * ROOK_SEMIOPEN_FILE_BONUS_EG;
      }

      // Rook on 7th rank (relative) is often decisive if the enemy king or pawns are confined.
      int r = rank_of(sq);
      if ((us == WHITE && r == 6) || (us == BLACK && r == 1)) {
        U64 oppPawn7 = (oppP & (0xFFULL << (r * 8))) != 0;
        bool oppKingBack = (us == WHITE) ? (rank_of(pos.kingSq[BLACK]) >= 6) : (rank_of(pos.kingSq[WHITE]) <= 1);
        if (oppPawn7 || oppKingBack) {
          mg += sign * ROOK_7TH_BONUS_MG;
          eg += sign * ROOK_7TH_BONUS_EG;
        }
      }
    }

    // Connected rooks (unblocked on same file/rank)
    if (popcount64(myR) >= 2) {
      int sqs[4];
      int n = 0;
      U64 tmp = myR;
      while (tmp && n < 4) sqs[n++] = pop_lsb(tmp);

      bool connected = false;
      for (int i = 0; i < n && !connected; i++) {
        for (int j = i + 1; j < n && !connected; j++) {
          int a = sqs[i], b = sqs[j];
          int fa = file_of(a), fb = file_of(b);
          int ra = rank_of(a), rb = rank_of(b);
          if (fa == fb) {
            int lo = std::min(ra, rb) + 1;
            int hi = std::max(ra, rb) - 1;
            U64 between = 0;
            for (int rr = lo; rr <= hi; rr++) between |= sq_bb(rr * 8 + fa);
            if ((between & occAll) == 0) connected = true;
          } else if (ra == rb) {
            int lo = std::min(fa, fb) + 1;
            int hi = std::max(fa, fb) - 1;
            U64 between = 0;
            for (int ff = lo; ff <= hi; ff++) between |= sq_bb(ra * 8 + ff);
            if ((between & occAll) == 0) connected = true;
          }
        }
      }
      if (connected) {
        mg += sign * CONNECTED_ROOKS_BONUS_MG;
        eg += sign * CONNECTED_ROOKS_BONUS_EG;
      }
    }
  }

  // Mobility (targets exclude own occupancy)
  auto mobility_side = [&](Color us, int& outMG, int& outEG){
    U64 myOcc = (us==WHITE) ? occW : occB;
    U64 targets = ~myOcc;

    U64 nbb = pos.bb[us][KNIGHT];
    while (nbb){
      int sq = pop_lsb(nbb);
      int m = popcount64(ATK.knight[sq] & targets);
      outMG += m * MOB_N_MG;
      outEG += m * MOB_N_EG;
    }

    U64 bbb = pos.bb[us][BISHOP];
    while (bbb){
      int sq = pop_lsb(bbb);
      int m = popcount64(bishop_attacks(sq, occAll) & targets);
      outMG += m * MOB_B_MG;
      outEG += m * MOB_B_EG;
    }

    U64 rbb = pos.bb[us][ROOK];
    while (rbb){
      int sq = pop_lsb(rbb);
      int m = popcount64(rook_attacks(sq, occAll) & targets);
      outMG += m * MOB_R_MG;
      outEG += m * MOB_R_EG;
    }

    U64 qbb = pos.bb[us][QUEEN];
    while (qbb){
      int sq = pop_lsb(qbb);
      int m = popcount64(queen_attacks(sq, occAll) & targets);
      outMG += m * MOB_Q_MG;
      outEG += m * MOB_Q_EG;
    }
  };

  int wMobMG=0,wMobEG=0,bMobMG=0,bMobEG=0;
  mobility_side(WHITE, wMobMG, wMobEG);
  mobility_side(BLACK, bMobMG, bMobEG);
  mg += (wMobMG - bMobMG);
  eg += (wMobEG - bMobEG);

  // Attack maps for threats + king safety ring
  U64 wAtt = attacks_for_side(pos, WHITE, occAll);
  U64 bAtt = attacks_for_side(pos, BLACK, occAll);

  // King ring pressure (MG)
  U64 wRing = ATK.king[pos.kingSq[WHITE]] | sq_bb(pos.kingSq[WHITE]);
  U64 bRing = ATK.king[pos.kingSq[BLACK]] | sq_bb(pos.kingSq[BLACK]);

  int wOnBRing = popcount64(wAtt & bRing);
  int bOnWRing = popcount64(bAtt & wRing);

  mg += (wOnBRing - bOnWRing) * KING_RING_ATTACK_W;
  if (wOnBRing >= KING_PRESSURE_TH) mg += KING_PRESSURE_BONUS;
  if (bOnWRing >= KING_PRESSURE_TH) mg -= KING_PRESSURE_BONUS;


// ------------------------------------------------------------
// King safety: "attack units" into an expanded king zone (MG)
// This is a classical, NNUE-free way to value coordinated attacks.
// ------------------------------------------------------------
auto king_zone = [&](int ksq) -> U64 {
  // Ring 1 (king moves) plus one extra ring.
  U64 r1 = ATK.king[ksq] | sq_bb(ksq);
  U64 r2 = 0;
  U64 tmp = r1;
  while (tmp) { int s = pop_lsb(tmp); r2 |= ATK.king[s]; }
  return r1 | r2;
};

auto attack_units = [&](Color att, int ksq, U64 occAllLocal) -> int {
  U64 zone = king_zone(ksq);
  int units = 0;
  int attackers = 0;

  // Pawns
  U64 pawns = pos.bb[att][PAWN];
  U64 pawnAtt = 0;
  while (pawns) { int sq = pop_lsb(pawns); pawnAtt |= ATK.pawn[att][sq]; }
  if (pawnAtt & zone) { units += 2; attackers++; }

  // Knights
  U64 knights = pos.bb[att][KNIGHT];
  while (knights) {
    int sq = pop_lsb(knights);
    U64 a = ATK.knight[sq] & zone;
    if (a) { units += g_params.ks_units_n; attackers++; }
  }

  // Bishops
  U64 bishops = pos.bb[att][BISHOP];
  while (bishops) {
    int sq = pop_lsb(bishops);
    U64 a = bishop_attacks(sq, occAllLocal) & zone;
    if (a) { units += g_params.ks_units_b; attackers++; }
  }

  // Rooks
  U64 rooks = pos.bb[att][ROOK];
  while (rooks) {
    int sq = pop_lsb(rooks);
    U64 a = rook_attacks(sq, occAllLocal) & zone;
    if (a) { units += g_params.ks_units_r; attackers++; }
  }

  // Queens
  U64 queens = pos.bb[att][QUEEN];
  while (queens) {
    int sq = pop_lsb(queens);
    U64 a = queen_attacks(sq, occAllLocal) & zone;
    if (a) { units += g_params.ks_units_q; attackers++; }
  }

  // Bonus for having multiple attacking pieces involved
  if (attackers >= 2) units += g_params.ks_attacker_bonus * (attackers - 1);
  if (g_params.ks_scale != 1) units *= g_params.ks_scale;
  return units;
};

auto ks_penalty = [&](int units)->int{
  // Convert units into a non-linear MG penalty. Clamp to keep it stable.
  static const int T[33] = {
    0,0,1,2,3,5,7,9,12,15,18,22,26,30,35,40,45,50,56,62,68,74,80,87,94,101,108,116,124,132,140,148,156
  };
  if (units < 0) units = 0;
  if (units > 32) units = 32;
  return T[units];
};

int wUnits = attack_units(WHITE, pos.kingSq[BLACK], occAll);
int bUnits = attack_units(BLACK, pos.kingSq[WHITE], occAll);
mg += ks_penalty(wUnits);
mg -= ks_penalty(bUnits);

  // Shield + open file safety (MG)
  U64 allP = pos.bb[WHITE][PAWN] | pos.bb[BLACK][PAWN];
  king_safety_shield_openfiles(pos, WHITE, pos.bb[WHITE][PAWN], allP, mg, +1);
  king_safety_shield_openfiles(pos, BLACK, pos.bb[BLACK][PAWN], allP, mg, -1);

  // Hanging piece penalties (MG/EG)
  hanging_penalties(pos, WHITE, wAtt, bAtt, mg, eg);
  hanging_penalties(pos, BLACK, bAtt, wAtt, mg, eg);


// Threats: reward creating attacked-and-undefended targets (MG/EG)
auto threats_bonus = [&](Color att, U64 attMap, U64 defMap, int& outMG, int& outEG) {
  Color def = (Color)(att ^ 1);
  int sign = (att == WHITE) ? +1 : -1;

  // Iterate enemy pieces (exclude king)
  for (int pt = KNIGHT; pt <= QUEEN; pt++) {
    U64 bbp = pos.bb[def][pt];
    while (bbp) {
      int sq = pop_lsb(bbp);
      U64 sbb = sq_bb(sq);
      if ((attMap & sbb) && !(defMap & sbb)) {
        int b = 0;
        if (pt == KNIGHT || pt == BISHOP) b = g_params.thr_hanging_minor;
        else if (pt == ROOK) b = g_params.thr_hanging_rook;
        else if (pt == QUEEN) b = g_params.thr_hanging_queen;
        outMG += sign * b;
        outEG += sign * (b / 2);
      }
    }
  }

  // Pawn threats (enemy pieces attacked by pawns)
  U64 pawns = pos.bb[att][PAWN];
  U64 pAtt = 0;
  while (pawns) { int sq = pop_lsb(pawns); pAtt |= ATK.pawn[att][sq]; }
  U64 targets = pAtt & (pos.occ[def] ^ pos.bb[def][KING]); // exclude king
  if (targets) {
    int n = popcount64(targets);
    outMG += sign * (n * 8);
    outEG += sign * (n * 4);
  }
};

threats_bonus(WHITE, wAtt, bAtt, mg, eg);
threats_bonus(BLACK, bAtt, wAtt, mg, eg);

  // Knight outposts
  for (int c=0;c<2;c++){
    Color us = (Color)c;
    int sign = (us==WHITE)? +1 : -1;
    U64 knights = pos.bb[us][KNIGHT];
    U64 myP = pos.bb[us][PAWN];
    U64 enP = pos.bb[us^1][PAWN];

    while (knights){
      int sq = pop_lsb(knights);
      int rFromUs = (us==WHITE) ? rank_of(sq) : (7 - rank_of(sq)); // 0..7

      // 5th/6th ranks from our side
      if (rFromUs < 4 || rFromUs > 5) continue;
      if (!supported_by_pawn(us, sq, myP)) continue;
      if (enemy_pawn_can_chase(us, sq, enP)) continue;

      mg += sign * OUTPOST_N_MG;
      eg += sign * OUTPOST_N_EG;
    }
  }

  // Bad bishop: pawns on same color as bishop
  auto bishop_color_pen = [&](Color c){
    int sign = (c==WHITE)? +1 : -1;
    U64 bishops = pos.bb[c][BISHOP];
    U64 pawns   = pos.bb[c][PAWN];

    while (bishops){
      int bSq = pop_lsb(bishops);
      bool bishopDark = (sq_bb(bSq) & DARK_SQ) != 0;
      int sameColorPawns = popcount64(pawns & (bishopDark ? DARK_SQ : LIGHT_SQ));
      mg -= sign * sameColorPawns * BAD_BISHOP_PEN_MG;
      eg -= sign * sameColorPawns * BAD_BISHOP_PEN_EG;
    }
  };
  bishop_color_pen(WHITE);
  bishop_color_pen(BLACK);

  // Tempo (small bias)
  mg += (pos.stm == WHITE) ? TEMPO_BONUS : -TEMPO_BONUS;

  // Endgame king activity (encourage central king when queens are off)
  // Scale by how far we are into the endgame.
  {
    const int endgame = (TOTAL_PHASE - phase);
    if (endgame > 0) {
      auto center_dist = [&](int sq) {
        int f = file_of(sq), r = rank_of(sq);
        int d1 = std::abs(f - 3) + std::abs(r - 3); // d4
        int d2 = std::abs(f - 4) + std::abs(r - 3); // e4
        int d3 = std::abs(f - 3) + std::abs(r - 4); // d5
        int d4 = std::abs(f - 4) + std::abs(r - 4); // e5
        return std::min(std::min(d1, d2), std::min(d3, d4));
      };
      int dw = center_dist(pos.kingSq[WHITE]);
      int db = center_dist(pos.kingSq[BLACK]);
      int bw = std::max(0, 4 - dw);
      int bb = std::max(0, 4 - db);
      int bonus = (endgame * 4) / TOTAL_PHASE;
      eg += (bw - bb) * bonus;
    }
  }
  eg += (pos.stm == WHITE) ? TEMPO_BONUS : -TEMPO_BONUS;


// Endgame scaling for drawish material (prevents over-optimism)
{
  int wp = popcount64(pos.bb[WHITE][PAWN]);
  int bp = popcount64(pos.bb[BLACK][PAWN]);
  int wq = popcount64(pos.bb[WHITE][QUEEN]);
  int bq = popcount64(pos.bb[BLACK][QUEEN]);
  int wr = popcount64(pos.bb[WHITE][ROOK]);
  int br = popcount64(pos.bb[BLACK][ROOK]);
  int wm = popcount64(pos.bb[WHITE][KNIGHT] | pos.bb[WHITE][BISHOP]);
  int bm = popcount64(pos.bb[BLACK][KNIGHT] | pos.bb[BLACK][BISHOP]);

  int scale = 64;
  if (wp + bp == 0 && wq + bq == 0 && wr + br == 0) {
    // Minor-only endgames are often very drawish.
    int minors = wm + bm;
    if (minors <= 2) scale = 8;
    else if (minors <= 4) scale = 20;
  } else if (wp + bp <= 2 && wq + bq == 0 && wr + br == 0) {
    // Almost pawnless minor endgames
    scale = 40;
  }
  // Apply only to the advantage component.
  mg = (mg * scale) / 64;
  eg = (eg * scale) / 64;
}

  // Tapered blend
  int mgPhase = phase;
  int egPhase = TOTAL_PHASE - phase;
  int score = (mg * mgPhase + eg * egPhase) / TOTAL_PHASE;

  // Return from side-to-move perspective for negamax
  return (pos.stm == WHITE) ? score : -score;
}


// ------------------------------------------------------------
// Eval cache (transposition-friendly static eval memoization)
// ------------------------------------------------------------
namespace {
  static constexpr size_t EVAL_TT_SIZE = 1u << 20; // 1M entries
  struct EvalEntry {
    uint64_t key = 0;
    int score = 0;
  };
  static EvalEntry EvalTT[EVAL_TT_SIZE];
}

int eval(const Position& pos) {
  const uint64_t k = pos.key;
  EvalEntry& e = EvalTT[k & (EVAL_TT_SIZE - 1)];
  if (e.key == k) return e.score;

  const int s = eval_uncached(pos);
  e.key = k;
  e.score = s;
  return s;
}

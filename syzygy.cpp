#include "syzygy.h"
#include "tbprobe.h"
#include "movelist.h"
#include <algorithm>

namespace syzygy {

static bool g_inited = false;
static bool g_enabled = false;
static std::string g_path;

static inline int popcount64(uint64_t x) {
#if defined(_MSC_VER)
  return (int)__popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

static inline void pos_to_tb(const Position& pos,
                             uint64_t& white, uint64_t& black,
                             uint64_t& kings, uint64_t& queens, uint64_t& rooks,
                             uint64_t& bishops, uint64_t& knights, uint64_t& pawns,
                             unsigned& rule50, unsigned& castling, unsigned& ep,
                             bool& turn) {
  white = pos.occ[WHITE];
  black = pos.occ[BLACK];
  kings = pos.bb[WHITE][KING] | pos.bb[BLACK][KING];
  queens = pos.bb[WHITE][QUEEN] | pos.bb[BLACK][QUEEN];
  rooks  = pos.bb[WHITE][ROOK]  | pos.bb[BLACK][ROOK];
  bishops= pos.bb[WHITE][BISHOP]| pos.bb[BLACK][BISHOP];
  knights= pos.bb[WHITE][KNIGHT]| pos.bb[BLACK][KNIGHT];
  pawns  = pos.bb[WHITE][PAWN]  | pos.bb[BLACK][PAWN];
  rule50 = (unsigned)std::min<int>(pos.halfmoveClock, 255);
  castling = (unsigned)(pos.castling & 15);
  ep = (pos.epSq == NO_SQ) ? 0u : (unsigned)pos.epSq; // 0 means none; ep squares are never 0 in real chess.
  turn = (pos.stm == WHITE);
}

bool init(const std::string& path) {
  g_path = path;
  g_inited = true;
  bool ok = tb_init(path.c_str());
  g_enabled = ok && (TB_LARGEST >= 3);
  return ok;
}

void free() {
  tb_free();
  g_enabled = false;
  g_inited = false;
  g_path.clear();
}

bool enabled() { return g_enabled; }
int largest() { return (int)TB_LARGEST; }

static bool within_limit(const Position& pos) {
  if (!g_enabled) return false;
  int pieces = popcount64(pos.occAll);
  return pieces <= (int)TB_LARGEST;
}

bool probe_wdl(const Position& pos, int& outWdl) {
  if (!within_limit(pos)) return false;

  uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
  unsigned rule50, castling, ep;
  bool turn;
  pos_to_tb(pos, white, black, kings, queens, rooks, bishops, knights, pawns, rule50, castling, ep, turn);

  unsigned wdl = tb_probe_wdl(white, black, kings, queens, rooks, bishops, knights, pawns,
                              rule50, castling, ep, turn);
  if (wdl == TB_RESULT_FAILED) return false;
  outWdl = (int)wdl;
  return true;
}

bool probe_root_dtz(const Position& pos, Move& outMove, int& outWdl, int& outDtz) {
  if (!within_limit(pos)) return false;

  uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
  unsigned rule50, castling, ep;
  bool turn;
  pos_to_tb(pos, white, black, kings, queens, rooks, bishops, knights, pawns, rule50, castling, ep, turn);

  unsigned res = tb_probe_root(white, black, kings, queens, rooks, bishops, knights, pawns,
                               rule50, castling, ep, turn, NULL);

  if (res == TB_RESULT_FAILED) return false;
  if (res == TB_RESULT_CHECKMATE || res == TB_RESULT_STALEMATE) {
    outMove = 0;
    outWdl = TB_GET_WDL(res);
    outDtz = (int)TB_GET_DTZ(res);
    return true;
  }

  // Decode into our internal Move by matching pseudo-legal moves.
  int from = (int)TB_GET_FROM(res);
  int to   = (int)TB_GET_TO(res);
  int prom = (int)TB_GET_PROMOTES(res);
  bool isEp = TB_GET_EP(res) != 0;

  Piece promoPiece = NO_PIECE;
  if (prom == TB_PROMOTES_QUEEN) promoPiece = QUEEN;
  else if (prom == TB_PROMOTES_ROOK) promoPiece = ROOK;
  else if (prom == TB_PROMOTES_BISHOP) promoPiece = BISHOP;
  else if (prom == TB_PROMOTES_KNIGHT) promoPiece = KNIGHT;

  MoveList ml;
  pos.gen_pseudo(ml);
  Move found = 0;

  for (int i=0;i<ml.size;i++) {
    Move m = ml.moves[i];
    if (m_from(m) != from || m_to(m) != to) continue;
    if (promoPiece != NO_PIECE) {
      if (!(m_flags(m) & MF_PROMO)) continue;
      if (m_promo(m) != promoPiece) continue;
    } else {
      if (m_flags(m) & MF_PROMO) continue;
    }
    if (isEp && !(m_flags(m) & MF_EP)) continue;
    if (!isEp && (m_flags(m) & MF_EP)) continue;

    // verify legality
    Position tmp = pos;
    Undo u;
    Color us = tmp.stm;
    tmp.make(m, u);
    bool legal = !tmp.is_attacked(tmp.kingSq[us], !us);
    tmp.unmake(m, u);
    if (!legal) continue;

    found = m;
    break;
  }

  if (!found) return false;

  outMove = found;
  outWdl = (int)TB_GET_WDL(res);
  outDtz = (int)TB_GET_DTZ(res);
  return true;
}

bool probe_root(const Position& pos, Move& outMove, int& outWdl) {
  int dtz = 0;
  return probe_root_dtz(pos, outMove, outWdl, dtz);
}

} // namespace syzygy

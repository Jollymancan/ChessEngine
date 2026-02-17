#include "search.h"
#include "eval.h"
#include "movelist.h"
#include "see.h"
#include "syzygy.h"
#include "params.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <unordered_map>

static std::string move_to_uci_local(Move m) {
  auto sq_to = [](int sq)->std::string{
    char f = char('a' + (sq & 7));
    char r = char('1' + (sq >> 3));
    return std::string() + f + r;
  };
  std::string s = sq_to(m_from(m)) + sq_to(m_to(m));
  if (m_flags(m) & MF_PROMO) {
    char pc = 'q';
    switch (m_promo(m)) {
      case KNIGHT: pc = 'n'; break;
      case BISHOP: pc = 'b'; break;
      case ROOK:   pc = 'r'; break;
      case QUEEN:  pc = 'q'; break;
      default: break;
    }
    s.push_back(pc);
  }
  return s;
}

static bool is_legal(Position& pos, Move m) {
  Undo u;
  Color us = pos.stm;
  pos.make(m, u);
  bool legal = !pos.is_attacked(pos.kingSq[us], !us);
  pos.unmake(m, u);
  return legal;
}

// Follow TT best moves to build a principal variation. This keeps PV generation
// lightweight (no PV arrays in the search stack) and robust to TT collisions.
static std::string build_pv(Position pos, Searcher& S, Move firstMove, int maxLen = 32) {
  std::vector<std::string> pv;
  pv.reserve(maxLen);

  // Track visited keys to avoid endless loops on TT collisions.
  std::vector<uint64_t> seen;
  seen.reserve(maxLen + 2);
  seen.push_back(pos.key);

  Move m = firstMove;
  for (int i = 0; i < maxLen && m != 0; i++) {
    if (!is_legal(pos, m)) break;

    Undo u;
    pos.make(m, u);
    pv.push_back(move_to_uci_local(m));

    // Break on repetition/cycle in PV.
    if (std::find(seen.begin(), seen.end(), pos.key) != seen.end()) break;
    seen.push_back(pos.key);

    TTEntry tte;
    if (!S.tt.probe(pos.key, tte) || tte.bestMove == 0) break;
    m = (Move)tte.bestMove;
  }

  std::string out;
  for (size_t i = 0; i < pv.size(); i++) {
    if (i) out.push_back(' ');
    out += pv[i];
  }
  return out;
}

static constexpr int INF  = SCORE_INF;
static constexpr int MATE = SCORE_MATE;

static inline bool is_capture(Move m) { return m_cap(m) != NO_PIECE || (m_flags(m) & MF_EP); }
static inline bool is_promo(Move m)   { return (m_flags(m) & MF_PROMO) != 0; }

static inline int piece_value(Piece p) {
  switch (p) {
    case PAWN: return 100;
    case KNIGHT: return 320;
    case BISHOP: return 330;
    case ROOK: return 500;
    case QUEEN: return 900;
    case KING: return 20000;
    default: return 0;
  }
}

static inline int sq_from_alg(const std::string& s) {
  if (s.size() != 2) return NO_SQ;
  int f = s[0] - 'a';
  int r = s[1] - '1';
  if ((unsigned)f > 7 || (unsigned)r > 7) return NO_SQ;
  return r*8 + f;
}

static inline int popcount64(uint64_t x) {
#if defined(_MSC_VER)
  return (int)__popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

Move parse_uci_move(Position& pos, const std::string& uci) {
  if (uci.size() < 4) return 0;
  int from = sq_from_alg(uci.substr(0,2));
  int to   = sq_from_alg(uci.substr(2,2));
  if (from == NO_SQ || to == NO_SQ) return 0;

  Piece promo = NO_PIECE;
  if (uci.size() >= 5) {
    char pc = (char)std::tolower((unsigned char)uci[4]);
    if (pc == 'q') promo = QUEEN;
    else if (pc == 'r') promo = ROOK;
    else if (pc == 'b') promo = BISHOP;
    else if (pc == 'n') promo = KNIGHT;
  }

  MoveList ml;
  pos.gen_pseudo(ml);
  for (int i=0;i<ml.size;i++) {
    Move m = ml.moves[i];
    if (m_from(m) != from || m_to(m) != to) continue;
    if (promo != NO_PIECE) {
      if (!(m_flags(m) & MF_PROMO)) continue;
      if (m_promo(m) != promo) continue;
    } else {
      if (m_flags(m) & MF_PROMO) continue;
    }
    // legality
    Undo u;
    Color us = pos.stm;
    pos.make(m,u);
    bool legal = !pos.is_attacked(pos.kingSq[us], !us);
    pos.unmake(m,u);
    if (!legal) continue;
    return m;
  }
  return 0;
}

Searcher::Searcher() {
  set_threads(1);
}

// -------------------- Heuristics tables (per thread) --------------------
void Searcher::Heuristics::clear() {
  std::fill(&killers[0][0], &killers[0][0] + Searcher::MAX_PLY*2, 0);
  std::fill(&history[0][0][0], &history[0][0][0] + 2*64*64, 0);
  std::fill(&countermove[0][0][0], &countermove[0][0][0] + 2*64*64, 0);
  std::fill(&contHist[0][0][0][0][0], &contHist[0][0][0][0][0] + 2*6*64*6*64, 0);
  std::fill(&captureHist[0][0][0], &captureHist[0][0][0] + 6*64*6, 0);
}

void Searcher::Heuristics::decay() {
  // Light decay to keep history/continuation from exploding and getting stale.
  // Multiply by 15/16.
  for (int s = 0; s < 2; s++) {
    for (int f = 0; f < 64; f++) {
      for (int t = 0; t < 64; t++) {
        int& h = history[s][f][t];
        h -= (h >> 4);
      }
    }
  }
  for (int s = 0; s < 2; s++) {
    for (int pp = 0; pp < 6; pp++) {
      for (int pto = 0; pto < 64; pto++) {
        for (int p = 0; p < 6; p++) {
          for (int to = 0; to < 64; to++) {
            int& ch = contHist[s][pp][pto][p][to];
            ch -= (ch >> 4);
          }
        }
      }
    }
  }
  for (int p = 0; p < 6; p++) {
    for (int to = 0; to < 64; to++) {
      for (int cp = 0; cp < 6; cp++) {
        int& ch = captureHist[p][to][cp];
        ch -= (ch >> 4);
      }
    }
  }

}

void Searcher::set_threads(int n) {
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  threads = n;
  heurByThread.resize((size_t)threads);
  // Don't wipe mid-game when resizing; but new threads should start clean.
  for (auto& h : heurByThread) {
    // If a newly-constructed entry, it is already zero-initialized; still ensure.
    // (And keep behavior deterministic across compilers.)
    h.clear();
  }
}

void Searcher::clear() {
  stopFlag.store(false);
  tt.clear();
  for (auto& h : heurByThread) h.clear();
}

void Searcher::stop() { stopFlag.store(true); }

void Searcher::tt_resize_mb(int mb) { tt.resize_mb(mb); }

void Searcher::set_syzygy_path(const std::string& path) {
  syzygyPath = path;
  if (!useSyzygy) return;
  syzygy::init(path);
}
void Searcher::set_book_file(const std::string& path) {
  if (path.empty()) {
    book.clear();
    std::cout << "info string book cleared" << std::endl;
    return;
  }
  if (book.load(path)) {
    std::cout << "info string book loaded " << book.filename()
              << " entries " << (uint64_t)book.entry_count() << std::endl;
  } else {
    std::cout << "info string book failed to load " << path << std::endl;
  }
  std::cout.flush();
}

Move Searcher::probe_book(Position& pos) const {
  lastBookWeight = 0;
  lastBookCandidates = 0;

  if (!useBook) return 0;
  if (!book.loaded()) return 0;

  auto hit = book.probe(pos, bookWeightedRandom, bookMinWeight);
  if (!hit) return 0;

  // Make sure the book move is legal in our move generator.
  Move m = parse_uci_move(pos, hit->uci);
  if (!m) return 0;

  lastBookWeight = hit->weight;
  lastBookCandidates = hit->candidates;
  return m;
}





static inline int wdl_to_score(int wdl, int ply) {
  // WDL: 0 LOSS, 1 BLESSED_LOSS, 2 DRAW, 3 CURSED_WIN, 4 WIN
  if (wdl == 4) return 10000 - ply;
  if (wdl == 3) return 9000 - ply;
  if (wdl == 2) return 0;
  if (wdl == 1) return -9000 + ply;
  return -10000 + ply;
}

struct StackFrame {
  Move pvMove = 0;
  Move prevMove = 0;
  int staticEval = 0;
};

static inline bool has_non_pawn_material(const Position& pos, Color c) {
  return (pos.bb[c][KNIGHT] | pos.bb[c][BISHOP] | pos.bb[c][ROOK] | pos.bb[c][QUEEN]) != 0;
}

static inline int move_score_basic(const Searcher::Heuristics& H, const Position& pos, Move m, Move ttMove, Move prevMove, int ply) {
  if (m == ttMove) return 10'000'000;
  if (is_capture(m) || is_promo(m)) {
    int victim = (m_flags(m) & MF_EP) ? PAWN : m_cap(m);
    int attacker = m_piece(m);
    // MVV-LVA style score (fast). We avoid doing full SEE here because this
    // function is called for *every* pseudo move at *every* node.
    int score = 5'000'000 + 1000 * (victim + 1) - attacker;
    // Capture history bonus (learns which captures tend to work)
    score += H.captureHist[attacker][m_to(m)][victim] * 4;

    // Promotions are very forcing; prioritize them heavily.
    if (is_promo(m)) {
      // m_promo() is the promoted piece type (KNIGHT..QUEEN)
      score += 400'000 + 50'000 * (int)m_promo(m);
    }

    // Recapture bonus: if responding to a capture on the same square, prioritize it.
    if (prevMove && is_capture(prevMove) && m_to(m) == m_to(prevMove)) score += 60'000;
    return score;
  }
  if (m == H.killers[ply][0]) return 4'000'000;
  if (m == H.killers[ply][1]) return 3'900'000;

  if (prevMove) {
    Move cm = H.countermove[pos.stm][m_from(prevMove)][m_to(prevMove)];
    if (m == cm) return 3'800'000;
  }

  int score = 0;
  score += H.history[pos.stm][m_from(m)][m_to(m)];
  if (prevMove) {
    int pp = m_piece(prevMove);
    int pto = m_to(prevMove);
    int p = m_piece(m);
    int to = m_to(m);
    score += H.contHist[pos.stm][pp][pto][p][to] / 2;
  }
  return score;
}

static inline void sort_moves(std::vector<std::pair<int,Move>>& v) {
  std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.first > b.first; });
}



// ---- Search heuristics helpers ----
static inline int clampi(int x, int lo, int hi){ return x < lo ? lo : (x > hi ? hi : x); }

// LMR reduction table based on a simple log formula.
static inline int lmr_reduction(int depth, int moveNum) {
  static int table[65][65];
  static bool init = false;
  if (!init) {
    for (int d = 0; d <= 64; d++)
      for (int m = 0; m <= 64; m++)
        table[d][m] = 0;
    for (int d = 1; d <= 64; d++) {
      for (int m = 1; m <= 64; m++) {
        double rd = (std::log((double)(d + 1)) * std::log((double)(m + 1))) / 2.25;
        int r = (int)rd;
        if (d <= 2) r = 0;
        table[d][m] = clampi(r, 0, d - 1);
      }
    }
    init = true;
  }
  depth = clampi(depth, 0, 64);
  moveNum = clampi(moveNum, 0, 64);
  return table[depth][moveNum];
}

struct SearchContext {
  Searcher* S = nullptr;
  Searcher::Heuristics* H = nullptr;
  bool allowSyzygy = true;
  std::chrono::steady_clock::time_point start;
  // Hard and soft time limits (ms since start).
  // Hard limit is used by time_up() and must never be exceeded.
  // Soft limit is used to decide whether to start another iteration.
  int hardLimitMs = 0;
  int softLimitMs = 0;
  int64_t nodes = 0;
  uint32_t timeCheck = 0; // for cheap periodic time checks
  int selDepth = 0;
  StackFrame stack[Searcher::MAX_PLY+1]{};
  uint64_t keyStack[Searcher::MAX_PLY+1]{};
  int rootHistoryLen = 0;
};

static inline bool time_up(SearchContext& ctx) {
  if (ctx.hardLimitMs <= 0) return false;
  // Checking the clock at every node is expensive. Poll periodically.
  if ((ctx.timeCheck++ & 2047u) != 0) return false; // ~every 2048 nodes
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.start).count();
  return ms >= ctx.hardLimitMs;
}

static inline bool repetition_draw(const Position& pos, const SearchContext& ctx, int ply) {
  // 50-move draw
  if (pos.is_draw_50move()) return true;

  // Threefold repetition: count occurrences of the *same* position (Zobrist key) with the
  // same side-to-move. Along a line, identical side-to-move positions repeat every 2 plies.
  const uint64_t k = pos.key;
  int occ = 1; // current node

  // Search line (root -> current): same side-to-move are 2 plies apart
  for (int i = ply - 2; i >= 0; i -= 2) {
    if (ctx.keyStack[i] == k) occ++;
    if (occ >= 3) return true;
  }

  // Game history before search started (exclude current root key at the end)
  // gameKeys contains the root key as the last element at search start.
  const int limit = std::max(0, ctx.rootHistoryLen - 1);
  for (int i = 0; i < limit; i++) {
    if (pos.gameKeys[i] == k) occ++;
    if (occ >= 3) return true;
  }

  return false;
}

static int qsearch(Position& pos, int alpha, int beta, int ply, SearchContext& ctx, Move prevMove, int qCheckDepth) {
  Searcher& S = *ctx.S;
  Searcher::Heuristics& H = *ctx.H;

  if (S.stopFlag.load() || time_up(ctx)) { S.stop(); return 0; }
  if (ply >= Searcher::MAX_PLY - 1) return eval(pos);

  // Mate distance pruning (tighten bounds)
  alpha = std::max(alpha, -MATE + ply);
  beta  = std::min(beta,  MATE - ply - 1);
  if (alpha >= beta) return alpha;
  ctx.nodes++;
  if (ply > ctx.selDepth) ctx.selDepth = ply;

  if (repetition_draw(pos, ctx, ply)) return 0;

  // If side to move is in check in quiescence, we must search evasions.
  bool inCheck = pos.is_attacked(pos.kingSq[pos.stm], !pos.stm);

  int stand = 0;
  if (!inCheck) {
    stand = eval(pos);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
  }

  MoveList ml;
  pos.gen_pseudo(ml);

  // Score captures/promotions, plus (very selectively) quiet checks at the first q ply.
  // Hot path: avoid heap allocations and full sorting.
  static constexpr int Q_MAX_MOVES = 256;
  Move moves[Q_MAX_MOVES];
  int  scores[Q_MAX_MOVES];
  int  count = 0;

  for (int i = 0; i < ml.size; i++) {
    Move m = ml.moves[i];
    const bool capOrPromo = is_capture(m) || is_promo(m);
    // If side is in check in quiescence, we must consider *all* evasions.
    // Otherwise consider captures/promotions, plus (very selectively) quiet checks.
    if (!capOrPromo && !inCheck && qCheckDepth <= 0) continue;

    if (capOrPromo) {
      // delta pruning: if even a winning capture can't raise alpha, skip
      int delta = 200 + (m_cap(m) != NO_PIECE ? piece_value(m_cap(m)) : 100);
      if (!inCheck && stand + delta < alpha) continue;

      // SEE pruning for losing captures
      if (!see_ge(pos, m, -50)) continue;

      int sc = move_score_basic(H, pos, m, 0, prevMove, ply);
      if (count < Q_MAX_MOVES) { moves[count] = m; scores[count] = sc; count++; }
      continue;
    }

    // Quiet check candidates (first q ply only): try move, keep only if it gives check.
    // When inCheck, this branch is used for quiet evasions too.
    Undo u;
    Color us0 = pos.stm;
    pos.make(m, u);
    bool legal = !pos.is_attacked(pos.kingSq[us0], !us0);
    bool givesCheck = false;
    if (legal) {
      Color them = !us0;
      givesCheck = pos.is_attacked(pos.kingSq[them], us0);
    }
    pos.unmake(m, u);
    if (!legal) continue;
    if (!inCheck && !givesCheck) continue;

    int sc = (inCheck ? 2'000'000 : 1'000'000) + H.history[pos.stm][m_from(m)][m_to(m)];
    // Prefer checks that look like good follow-ups (continuation history)
    if (prevMove) {
      int pp = m_piece(prevMove);
      int pto = m_to(prevMove);
      int p = m_piece(m);
      int to = m_to(m);
      sc += H.contHist[pos.stm][pp][pto][p][to] / 4;
    }
    if (count < Q_MAX_MOVES) { moves[count] = m; scores[count] = sc; count++; }
  }

  // Order moves by score (descending) using selection to avoid allocations.
  for (int i = 0; i < count; i++) {
    int best = i;
    for (int j = i + 1; j < count; j++) if (scores[j] > scores[best]) best = j;
    if (best != i) { std::swap(scores[i], scores[best]); std::swap(moves[i], moves[best]); }
  }

  // If we enabled quiet checks, cap how many of them we try (after ordering).
  // Never apply this cap when in check (evasions must be fully searched).
  if (qCheckDepth > 0 && !inCheck) {
    Move filtered[Q_MAX_MOVES];
    int  fscores[Q_MAX_MOVES];
    int  fcount = 0;
    int keptChecks = 0;

    for (int i = 0; i < count; i++) {
      Move m = moves[i];
      bool quiet = !is_capture(m) && !is_promo(m);
      if (quiet) {
        if (keptChecks++ >= 8) continue;
      }
      filtered[fcount] = m;
      fscores[fcount] = scores[i];
      fcount++;
    }
    for (int i = 0; i < fcount; i++) { moves[i] = filtered[i]; scores[i] = fscores[i]; }
    count = fcount;
  }

Color us = pos.stm;
  for (int mi = 0; mi < count; mi++) {
    Move m = moves[mi];
    Undo u;
    pos.make(m,u);
    bool legal = !pos.is_attacked(pos.kingSq[us], !us);
    if (!legal) { pos.unmake(m,u); continue; }

    ctx.keyStack[ply+1] = pos.key;
    // We only allow quiet checks at the first q ply. Deeper qsearch is captures/promotions only.
    int score = -qsearch(pos, -beta, -alpha, ply+1, ctx, m, 0);
    pos.unmake(m,u);

    if (S.stopFlag.load()) return 0;

    if (score >= beta) return beta;
    if (score > alpha) alpha = score;
  }

  return alpha;
}

static int negamax(Position& pos, int alpha, int beta, int depth, int ply, bool pvNode,
                   Move prevMove, SearchContext& ctx, Move excludedMove = 0, bool allowIID = true) {
  Searcher& S = *ctx.S;
  Searcher::Heuristics& H = *ctx.H;

  if (S.stopFlag.load() || time_up(ctx)) { S.stop(); return 0; }
  if (ply >= Searcher::MAX_PLY - 1) return eval(pos);

  // Mate distance pruning (tighten bounds)
  alpha = std::max(alpha, -MATE + ply);
  beta  = std::min(beta,  MATE - ply - 1);
  if (alpha >= beta) return alpha;

  ctx.nodes++;
  if (ply > ctx.selDepth) ctx.selDepth = ply;

  bool inCheck = pos.is_attacked(pos.kingSq[pos.stm], !pos.stm);

  // Check extension
  if (inCheck) depth++;

  // Draws
  if (!inCheck && repetition_draw(pos, ctx, ply)) return 0;

  // Syzygy WDL
  if (S.useSyzygy && ctx.allowSyzygy && syzygy::enabled()) {
    int pieces = popcount64(pos.occAll);
    if (pieces <= syzygy::largest()) {
      int wdl;
      if (syzygy::probe_wdl(pos, wdl)) {
        return wdl_to_score(wdl, ply);
      }
    }
  }

  if (depth <= 0) return qsearch(pos, alpha, beta, ply, ctx, prevMove, 1);

  // TT probe
  TTEntry tte;
  bool ttHit = false;
  int ttScore = 0;
  Move ttMove = 0;
  if (S.tt.probe(pos.key, tte)) {
    ttHit = true;
    ttMove = (Move)tte.bestMove;
    ttScore = S.tt.unpack_score((int)tte.score, ply);
    if (tte.depth >= depth && !pvNode) {
      if (tte.flag == TT_EXACT) return ttScore;
      if (tte.flag == TT_ALPHA && ttScore <= alpha) return alpha;
      if (tte.flag == TT_BETA  && ttScore >= beta)  return beta;
    }
  }

  // PV bound tightening: even when we can't return immediately, we can use
  // the stored bound to narrow the window and speed up the PV search.
  if (ttHit && tte.depth >= depth && pvNode && tte.flag != TT_EXACT) {
    if (tte.flag == TT_ALPHA) {
      beta = std::min(beta, ttScore);
    } else if (tte.flag == TT_BETA) {
      alpha = std::max(alpha, ttScore);
    }
    if (alpha >= beta) return alpha;
  }

  // Internal Iterative Deepening (IID): if we have no TT move at a PV node,
  // do a shallow search to seed a good move for ordering.
  if (allowIID && pvNode && !inCheck && depth >= 6 && ttMove == 0) {
    int iidDepth = depth - 2;
    if (iidDepth > 0) {
      (void)negamax(pos, alpha, beta, iidDepth, ply, true, prevMove, ctx, 0, false);
      if (S.stopFlag.load()) return 0;
      TTEntry t2;
      if (S.tt.probe(pos.key, t2)) ttMove = (Move)t2.bestMove;
    }
  }

  int origAlpha = alpha;

  // Static eval for pruning (only when not in check).
  // This is a hot path, so avoid calling eval() if we won't use it.
  int staticEval = 0;
  bool improving = false;
  if (!inCheck) {
    staticEval = eval(pos);
    ctx.stack[ply].staticEval = staticEval;
    improving = (ply >= 2 && staticEval > ctx.stack[ply-2].staticEval);
  } else {
    ctx.stack[ply].staticEval = 0;
  }

  // Reverse futility
  if (!pvNode && !inCheck && depth <= 3) {
    static const int margin[4] = {0, 120, 240, 400};
    if (staticEval - margin[depth] >= beta) return staticEval - margin[depth];
  }

  // Razoring
  if (!pvNode && !inCheck && depth <= 2) {
    static const int razor[3] = {0, 220, 420};
    if (staticEval + razor[depth] <= alpha) {
      return qsearch(pos, alpha, beta, ply, ctx, prevMove, 1);
    }
  }

  // Null move pruning (verified for deeper nodes)
  if (!pvNode && !inCheck && depth >= 3 && has_non_pawn_material(pos, pos.stm) && pos.halfmoveClock < 90) {
    int R = 2 + (depth >= 6);
    Undo u;
    pos.make_null(u);
    ctx.keyStack[ply+1] = pos.key;
    int score = -negamax(pos, -beta, -beta+1, depth - 1 - R, ply+1, false, 0, ctx, 0, false);
    pos.unmake_null(u);
    if (S.stopFlag.load()) return 0;

    if (score >= beta) {
      if (depth >= 8) {
        // Verification search from the original position at reduced depth
        int vscore = negamax(pos, beta-1, beta, depth - 1 - R, ply, false, prevMove, ctx, 0, false);
        if (S.stopFlag.load()) return 0;
        if (vscore >= beta) return beta;
      } else {
        return beta;
      }
    }
  }

  // Singular extension (conservative): if TT move looks uniquely strong, extend it by 1 ply.
  bool singularExtend = false;
  if (allowIID && pvNode && !inCheck && ttHit && ttMove && depth >= 8 && tte.depth >= depth - 2 && tte.flag == TT_EXACT) {
    int singMargin = 2 * depth + 50;
    int singBeta = ttScore - singMargin;
    int singDepth = depth - 4;
    if (singDepth > 0) {
      int others = negamax(pos, singBeta - 1, singBeta, singDepth, ply, false, prevMove, ctx, ttMove, false);
      if (S.stopFlag.load()) return 0;
      if (others < singBeta) singularExtend = true;
    }
  }

  // ProbCut: try a few good captures at reduced depth to see if we can prove a beta cutoff.
  if (!pvNode && !inCheck && depth >= 6 && beta < MATE - 1000 && beta > -MATE + 1000) {
    int margin = 80 + 20 * depth;
    int pcBeta = beta + margin;
    int pcDepth = depth - 4;
    if (pcDepth > 0) {
      MoveList pc;
      pos.gen_pseudo(pc);
      std::vector<std::pair<int,Move>> caps;
      caps.reserve(pc.size);
      for (int i = 0; i < pc.size; i++) {
        Move m = pc.moves[i];
        if (!is_capture(m) && !is_promo(m)) continue;
        if (!see_ge(pos, m, 0)) continue;
        caps.push_back({move_score_basic(H, pos, m, ttMove, prevMove, ply), m});
      }
      sort_moves(caps);
      int tried = 0;
      Color us = pos.stm;
      for (auto& sm : caps) {
        if (tried++ >= 6) break;
        Move m = sm.second;
        Undo u;
        pos.make(m, u);
        bool legal = !pos.is_attacked(pos.kingSq[us], !us);
        if (!legal) { pos.unmake(m, u); continue; }
        ctx.keyStack[ply+1] = pos.key;
        int score = -negamax(pos, -pcBeta, -(pcBeta - 1), pcDepth, ply+1, false, m, ctx, 0, false);
        pos.unmake(m, u);
        if (S.stopFlag.load()) return 0;
        if (score >= pcBeta) return beta;
      }
    }
  }

  // Generate and order moves
  MoveList ml;
  pos.gen_pseudo(ml);

  // Hot path: avoid allocations and full sorting in every node.
  static constexpr int N_MAX_MOVES = 256;
  Move moves[N_MAX_MOVES];
  int  scores[N_MAX_MOVES];
  int  count = 0;

  for (int i = 0; i < ml.size; i++) {
    Move m = ml.moves[i];
    if (count < N_MAX_MOVES) {
      moves[count] = m;
      scores[count] = move_score_basic(H, pos, m, ttMove, prevMove, ply);
      count++;
    }
  }

  // Order moves by score (descending) using selection to avoid heap traffic.
  for (int i = 0; i < count; i++) {
    int best = i;
    for (int j = i + 1; j < count; j++) if (scores[j] > scores[best]) best = j;
    if (best != i) { std::swap(scores[i], scores[best]); std::swap(moves[i], moves[best]); }
  }

  Color us = pos.stm;
  Move bestMove = 0;
  int bestScore = -INF;
  int legalMoves = 0;

  // Late move pruning thresholds
  auto lmp_limit = [&](int d)->int{
    if (d <= 1) return 6;
    if (d == 2) return 10;
    if (d == 3) return 16;
    return 999;
  };

  for (int idx = 0; idx < count; idx++) {
    Move m = moves[idx];
    if (excludedMove && m == excludedMove) continue;

    // LMP for quiet moves
    if (!pvNode && !inCheck && depth <= 3 && legalMoves >= lmp_limit(depth) && !is_capture(m) && !is_promo(m)) {
      continue;
    }

    // Main-node SEE pruning for obviously losing captures (helps avoid tactical noise).
    // Keep TT move and promotions; don't apply in PV or in check.
    if (!pvNode && !inCheck && is_capture(m) && !is_promo(m) && m != ttMove) {
      // More aggressive at shallow depth.
      int thr = (depth <= 3 ? -50 : -100);
      if (!see_ge(pos, m, thr)) {
        continue;
      }
    }

    // Futility pruning (quiet moves only)
    if (!pvNode && !inCheck && depth <= 3 && !is_capture(m) && !is_promo(m)) {
      static const int fm[4] = {0, 90, 170, 260};
      if (staticEval + fm[depth] <= alpha) {
        // Keep checks (very crude: if move gives check, don't prune)
        Undo tu;
        pos.make(m, tu);
        bool givesCheck = pos.is_attacked(pos.kingSq[!us], us);
        pos.unmake(m, tu);
        if (!givesCheck) continue;
      }
    }

    Undo u;
    pos.make(m,u);
    bool legal = !pos.is_attacked(pos.kingSq[us], !us);
    if (!legal) { pos.unmake(m,u); continue; }

	    // Safe history pruning (quiet moves only). IMPORTANT: apply only after legality is known,
	    // and never prune checking moves. This avoids "depth but blind" behavior.
	    if (!pvNode && !inCheck && depth >= g_params.hist_prune_min_depth && !is_capture(m) && !is_promo(m) && m != ttMove) {
	      // Only prune very late moves (depth-scaled)
	      const int late = g_params.hist_prune_late_base + depth * g_params.hist_prune_late_per_depth;
	      if (idx >= late) {
	        const bool givesCheck = pos.is_attacked(pos.kingSq[!us], us);
	        if (!givesCheck && m != H.killers[ply][0] && m != H.killers[ply][1]) {
	          bool isCM = false;
	          int cont = 0;
	          if (prevMove) {
	            Move cm = H.countermove[us][m_from(prevMove)][m_to(prevMove)];
	            isCM = (m == cm);
	            int pp = m_piece(prevMove);
	            int pto = m_to(prevMove);
	            int p  = m_piece(m);
	            int to = m_to(m);
	            cont = H.contHist[us][pp][pto][p][to] / 2;
	          }
	          if (!isCM) {
	            int h = H.history[us][m_from(m)][m_to(m)] + cont;
	            // Require *very* negative history to prune.
	            if (h < g_params.hist_prune_threshold) { pos.unmake(m,u); continue; }
	          }
	        }
	      }
	    }

    legalMoves++;
    ctx.keyStack[ply+1] = pos.key;

    bool childPv = pvNode && (legalMoves == 1);
    int newDepth = depth - 1 + ((singularExtend && m == ttMove) ? 1 : 0);

    int score = 0;

    // PVS + LMR (quiet moves)
    bool quiet = !is_capture(m) && !is_promo(m);

    if (legalMoves == 1) {
      // First move: always full window
      score = -negamax(pos, -beta, -alpha, newDepth, ply+1, childPv, m, ctx);
    } else {
      // Late Move Reductions for quiet moves
      int rd = newDepth;
      if (!childPv && quiet && !inCheck && newDepth >= 3 && legalMoves >= 4) {
        int r = lmr_reduction(depth, legalMoves);
        // Improve-based LMR: reduce less when improving, reduce more when not.
        if (improving) r = std::max(0, r - 1);

        // Reduce less for checking moves (tactical)
        const bool givesCheck = pos.is_attacked(pos.kingSq[!us], us);
        if (givesCheck) r = std::max(0, r - g_params.lmr_check_bonus);

        // Use history/continuation to adjust reductions.
        int hist = H.history[us][m_from(m)][m_to(m)];
        if (prevMove) {
          int pp = m_piece(prevMove);
          int pto = m_to(prevMove);
          int p  = m_piece(m);
          int to = m_to(m);
          hist += H.contHist[us][pp][pto][p][to] / 2;
        }
        if (hist > 2000) r = std::max(0, r - g_params.lmr_goodhist_bonus);
        if (hist < -500) r += g_params.lmr_badhist_penalty;

        // Protect killer/countermove a bit (often tactical).
        if (m == H.killers[ply][0] || m == H.killers[ply][1]) r = std::max(0, r - 1);
        if (prevMove) {
          Move cm = H.countermove[us][m_from(prevMove)][m_to(prevMove)];
          if (m == cm) r = std::max(0, r - 1);
        }
        r = std::min(r, newDepth - 1);
        rd = newDepth - r;
      }

      // Reduced null-window search
      score = -negamax(pos, -alpha-1, -alpha, rd, ply+1, false, m, ctx, 0, false);
      if (score > alpha && !S.stopFlag.load()) {
        // Re-search at full depth
        score = -negamax(pos, -beta, -alpha, newDepth, ply+1, childPv, m, ctx, 0, false);
      }
    }

    pos.unmake(m,u);

    if (S.stopFlag.load()) return 0;

    if (score > bestScore) {
      bestScore = score;
      bestMove = m;
    }

    if (score > alpha) {
      alpha = score;
      // update pv-ish
    }

    if (alpha >= beta) {
      // beta cutoff updates for quiet moves
      if (!is_capture(m) && !is_promo(m)) {
        if (H.killers[ply][0] != m) {
          H.killers[ply][1] = H.killers[ply][0];
          H.killers[ply][0] = m;
        }
        int bonus = depth * depth;
        int from = m_from(m), to = m_to(m);
        H.history[us][from][to] += bonus;

        if (prevMove) {
          H.countermove[us][m_from(prevMove)][m_to(prevMove)] = m;
          int pp = m_piece(prevMove);
          int pto = m_to(prevMove);
          int p  = m_piece(m);
          H.contHist[us][pp][pto][p][to] += bonus;
        }
      } else {
        // Beta cutoff updates for captures/promotions (capture history)
        int attacker = m_piece(m);
        int victim = (m_flags(m) & MF_EP) ? PAWN : m_cap(m);
        if (victim != NO_PIECE) {
          int bonus = depth * depth;
          H.captureHist[attacker][m_to(m)][victim] += bonus;
        }
      }

      // store TT beta
      S.tt.store(pos.key, depth, S.tt.pack_score(beta, ply), TT_BETA, (uint32_t)m);
      return beta;
    }
  }

  if (legalMoves == 0) {
    // checkmate or stalemate
    if (inCheck) return -MATE + ply;
    return 0;
  }

  // store TT
  uint8_t flag = (alpha <= origAlpha) ? TT_ALPHA : TT_EXACT;
  S.tt.store(pos.key, depth, S.tt.pack_score(alpha, ply), flag, (uint32_t)bestMove);

  // Expose best move for the current ply (useful at root even if TT collides)
  ctx.stack[ply].pvMove = bestMove;

  return alpha;
}

static inline int compute_time_limit_ms(const Position& pos, const GoLimits& lim) {
  if (lim.movetime_ms > 0) return lim.movetime_ms;
  if (lim.depth > 0) return 0;

  int time = (pos.stm == WHITE) ? lim.wtime_ms : lim.btime_ms;
  int inc  = (pos.stm == WHITE) ? lim.winc_ms  : lim.binc_ms;

  if (time <= 0) return 0;

  // Moves to go: UCI might not provide it. Use a conservative default.
  int mtg = (lim.movestogo > 0) ? lim.movestogo : 30;
  mtg = std::max(5, std::min(70, mtg));

  // If we're in real time trouble, keep it very small.
  if (time < 1500) {
    int limMs = std::max(5, time / 12 + inc / 2);
    return std::min(limMs, std::max(5, time / 3));
  }

  // Base budget: a slice of remaining time + most of the increment.
  // Using (mtg + 6) gives more stable allocations than time/mtg.
  double base = (double)time / (double)(mtg + 6);
  base += 0.75 * (double)inc;

  // Phase scaling: spend a bit more in the opening, a bit less in late endgames.
  int fm = (int)pos.fullmoveNumber;
  if (fm <= 12) base *= 1.15;
  else if (fm >= 40) base *= 0.95;

  // Endgame scaling: if very few pieces remain, avoid over-investing.
  int pieces = popcount64(pos.occAll);
  if (pieces <= 10) base *= 0.85;

  int limit = (int)base;
  // Never use too much of remaining time (hard safety)
  limit = std::min(limit, time / 2);
  // Never use too little
  limit = std::max(limit, 5);
  return limit;
}

Move Searcher::go(Position& pos, const GoLimits& lim) {
  stopFlag.store(false);
  tt.new_search();

  if (useSyzygy && !syzygyPath.empty()) {
    syzygy::init(syzygyPath);
  }

  // Syzygy root probe (fast)
  if (useSyzygy && syzygy::enabled()) {
    Move tbMove;
    int wdl;
    int dtz = 0;
    if (syzygy::probe_root_dtz(pos, tbMove, wdl, dtz) && tbMove != 0) {
      std::cout << "info string syzygy root move " << move_to_uci_local(tbMove)
                << " wdl " << wdl << " dtz " << dtz << std::endl;
      std::cout.flush();
      return tbMove;
    }
  }

  // Polyglot opening book (root-only).
  // This should be checked after Syzygy root probe.
  // We only use the book in the first N plies of the *game*.
  auto game_ply = [&](const Position& p) -> int {
    int fm = (int)p.fullmoveNumber;
    if (fm < 1) fm = 1;
    int ply = (fm - 1) * 2 + (p.stm == BLACK ? 1 : 0);
    if (ply < 0) ply = 0;
    return ply;
  };

  if (useBook && book.loaded()) {
    int ply = game_ply(pos);
    if (ply <= bookMaxPly) {
      Move bm = probe_book(pos);
      if (bm) {
        std::cout << "info string book move " << move_to_uci_local(bm)
                  << " weight " << (int)lastBookWeight
                  << " candidates " << lastBookCandidates
                  << " ply " << ply << std::endl;
        std::cout.flush();
        return bm;
      }
    }
  }

  SearchContext ctx;
  ctx.S = this;
  ctx.H = &heurByThread[0];
  ctx.allowSyzygy = true;
  ctx.start = std::chrono::steady_clock::now();
  // Compute hard/soft time limits.
  // For movetime: use (movetime - overhead) as hard, and stop at soft if PV is stable.
  // For clock mode: compute a budget and keep a small safety margin.
  int baseLimit = compute_time_limit_ms(pos, lim);
  if (baseLimit > 0) {
    int hard = baseLimit - moveOverheadMs;
    hard = std::max(1, hard);
    // Soft limit: aim to stop a bit early to avoid overrun and to stop at clean iteration boundaries.
    // Keep this modest so we still use nearly all allotted time for difficult positions.
    int softSlack = std::min(1000, std::max(50, hard / 20)); // ~5% (capped at 1s)
    int soft = std::max(1, hard - softSlack);
    ctx.hardLimitMs = hard;
    ctx.softLimitMs = soft;
  } else {
    ctx.hardLimitMs = 0;
    ctx.softLimitMs = 0;
  }
  ctx.nodes = 0;
  ctx.rootHistoryLen = (int)pos.gameKeys.size();
  if (ctx.rootHistoryLen > (int)pos.gameKeys.size()) ctx.rootHistoryLen = (int)pos.gameKeys.size();
  ctx.keyStack[0] = pos.key;

  Move best = 0;
  int bestScore = -INF;

  int maxD = (lim.depth > 0) ? lim.depth : 64;
  if (maxDepth > 0) maxD = std::min(maxD, maxDepth);

  // -------------------- Lazy SMP (multi-thread search) --------------------
  const int nThreads = std::max(1, threads);
  // Ensure per-thread heuristic tables exist.
  if ((int)heurByThread.size() != nThreads) {
    // Avoid silently resizing without clearing; treat this as a user misconfiguration.
    set_threads(nThreads);
    ctx.H = &heurByThread[0];
  }

  std::atomic<int> sharedDepth{0};
  std::vector<std::thread> workers;
  workers.reserve(std::max(0, nThreads - 1));

  if (nThreads > 1) {
    // Helper threads search slightly behind the main thread to populate the TT.
    for (int t = 1; t < nThreads; t++) {
      Position rootCopy = pos; // independent copy
      workers.emplace_back([this, t, root = std::move(rootCopy), &sharedDepth,
                            start = ctx.start, hard = ctx.hardLimitMs, soft = ctx.softLimitMs,
                            maxD]() mutable {
        SearchContext hctx;
        hctx.S = this;
        hctx.H = &heurByThread[t];
        hctx.allowSyzygy = false; // avoid concurrent TB probing
        hctx.start = start;
        hctx.hardLimitMs = hard;
        hctx.softLimitMs = soft;
        hctx.nodes = 0;
        hctx.selDepth = 0;
        hctx.rootHistoryLen = (int)root.gameKeys.size();
        hctx.keyStack[0] = root.key;

        int last = 0;
        while (!stopFlag.load(std::memory_order_relaxed)) {
          int d = sharedDepth.load(std::memory_order_relaxed);
          if (d <= 1) { std::this_thread::yield(); continue; }
          int sd = std::max(1, std::min(maxD, d - 1));
          if (sd == last) { std::this_thread::yield(); continue; }
          last = sd;

          hctx.selDepth = 0;
          // Full-window PV search; results are only used via TT.
          (void)negamax(root, -INF, INF, sd, 0, true, 0, hctx);
        }
      });
    }
  }

  Move prevIterBest = 0;
  int prevIterScore = 0;
  int stableCount = 0;

  // Root ordering persistence across iterations (helps fixed-time strength).
  std::unordered_map<uint32_t, int> rootScoreHint;

  for (int depth=1; depth<=maxD; depth++) {
    if (stopFlag.load()) break;

    // Let helper threads know what depth we're starting.
    sharedDepth.store(depth, std::memory_order_relaxed);

    // Reset selDepth per iteration (UCI convention).
    ctx.selDepth = 0;

    // If we're already past the soft limit and the PV has been stable, don't start a new depth.
    if (ctx.softLimitMs > 0) {
      auto now = std::chrono::steady_clock::now();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.start).count();
      if (ms >= ctx.softLimitMs && stableCount >= 2 && prevIterBest != 0)
        break;
    }

    auto print_info = [&](int multipvIdx, int score, const std::string& pv) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx.start).count();
      if (ms < 1) ms = 1;
      const int nps = (int)((ctx.nodes * 1000) / ms);
      const int hf = tt.hashfull();

      std::cout << "info depth " << depth
                << " seldepth " << ctx.selDepth
                << " multipv " << multipvIdx
                << " score ";

      if (score > MATE - 1000) {
        int mate = MATE - score;
        if (mate < 1) mate = 1;
        std::cout << "mate " << mate;
      } else if (score < -MATE + 1000) {
        int mate = -(MATE + score);
        if (mate > -1) mate = -1;
        std::cout << "mate " << mate;
      } else {
        std::cout << "cp " << score;
      }

      std::cout << " nodes " << ctx.nodes
                << " nps " << nps
                << " hashfull " << hf
                << " time " << ms;

      if (!pv.empty()) std::cout << " pv " << pv;
      std::cout << std::endl;
    };

    // MultiPV analysis: score each root move independently (slower, but accurate and simple).
    if (multiPV > 1) {
      struct RootLine { Move m; int score; };
      std::vector<std::pair<int,Move>> rootMoves;
      rootMoves.reserve(64);

      // Seed ordering with TT move (if any)
      TTEntry rt;
      Move ttMove = 0;
      if (tt.probe(pos.key, rt) && rt.bestMove) ttMove = (Move)rt.bestMove;

      MoveList ml;
      pos.gen_pseudo(ml);
      for (int i = 0; i < ml.size; i++) {
        Move m = ml.moves[i];
        // Basic legality check (cheaper than full make/unmake for ordering, but we verify later)
        int sc = move_score_basic(heurByThread[0], pos, m, ttMove, 0, 0);
        rootMoves.push_back({sc, m});
      }
      sort_moves(rootMoves);

      std::vector<RootLine> lines;
      lines.reserve(rootMoves.size());

      Color us = pos.stm;
      for (auto& sm : rootMoves) {
        Move m = sm.second;
        Undo u;
        pos.make(m, u);
        bool legal = !pos.is_attacked(pos.kingSq[us], !us);
        if (!legal) { pos.unmake(m, u); continue; }
        ctx.keyStack[1] = pos.key;

        int sc = -negamax(pos, -INF, INF, depth - 1, 1, true, m, ctx);
        pos.unmake(m, u);
        if (stopFlag.load()) break;
        lines.push_back({m, sc});
      }

      if (stopFlag.load() || lines.empty()) break;

      std::sort(lines.begin(), lines.end(), [](const RootLine& a, const RootLine& b) {
        return a.score > b.score;
      });

      best = lines[0].m;
      bestScore = lines[0].score;

      // Print top-N PV lines.
      const int count = std::min<int>(multiPV, (int)lines.size());
      for (int i = 0; i < count; i++) {
        std::string pv = build_pv(pos, *this, lines[i].m, 32);
        print_info(i + 1, lines[i].score, pv);
      }

    } else {
      int score = 0;

      // Root search:
      // - threads==1: aspiration window PV search
      // - threads>1: parallel root move scoring (safe, strong for fixed-time)

      if (nThreads <= 1 || depth == 1) {
        // Aspiration windows (depth >= 2). Use a depth-scaled window.
        if (depth == 1) {
          score = negamax(pos, -INF, INF, depth, 0, true, 0, ctx);
        } else {
          int center = bestScore;
          int window = g_params.asp_base + depth * g_params.asp_per_depth;
          int alpha = center - window;
          int beta  = center + window;

          for (int tries = 0; tries < 5; tries++) {
            score = negamax(pos, alpha, beta, depth, 0, true, 0, ctx);
            if (stopFlag.load()) break;

            if (score <= alpha) {
              window = window * 2 + 10;
              alpha = center - window;
              beta  = center + window;
              continue;
            }
            if (score >= beta) {
              window = window * 2 + 10;
              alpha = center - window;
              beta  = center + window;
              continue;
            }
            break;
          }

          if (!stopFlag.load() && (score <= alpha || score >= beta)) {
            score = negamax(pos, -INF, INF, depth, 0, true, 0, ctx);
          }
        }
      } else {
        // Parallel root scoring (similar to MultiPV but parallelized).
        struct RootJob { Move m; int order; };
        std::vector<RootJob> jobs;
        jobs.reserve(64);

        TTEntry rt;
        Move ttMove = 0;
        if (tt.probe(pos.key, rt) && rt.bestMove) ttMove = (Move)rt.bestMove;

        MoveList ml;
        pos.gen_pseudo(ml);

        for (int i = 0; i < ml.size; i++) {
          Move m = ml.moves[i];
          int sc = move_score_basic(heurByThread[0], pos, m, ttMove, 0, 0);
          auto it = rootScoreHint.find((uint32_t)m);
          if (it != rootScoreHint.end()) sc += it->second * 4; // light bias
          jobs.push_back({m, sc});
        }

        std::sort(jobs.begin(), jobs.end(), [](const RootJob& a, const RootJob& b){ return a.order > b.order; });

        // Filter legal moves and build move list.
        std::vector<Move> moves;
        moves.reserve(jobs.size());
        {
          Color us = pos.stm;
          for (auto& j : jobs) {
            Undo u;
            pos.make(j.m, u);
            bool legal = !pos.is_attacked(pos.kingSq[us], !us);
            pos.unmake(j.m, u);
            if (legal) moves.push_back(j.m);
          }
        }

        if (moves.empty()) {
          score = negamax(pos, -INF, INF, depth, 0, true, 0, ctx);
        } else {
          std::atomic<int> next{0};
          std::atomic<int> bestSc{-INF};
          std::atomic<uint32_t> bestMv{0};
          std::atomic<uint64_t> totalNodes{0};
          std::vector<std::thread> rootWorkers;
          rootWorkers.reserve(nThreads - 1);

          auto worker_fn = [&](int tid) {
            Position root = pos;
            SearchContext lctx;
            lctx.S = this;
            lctx.H = &heurByThread[tid];
            lctx.allowSyzygy = (tid == 0);
            lctx.start = ctx.start;
            lctx.hardLimitMs = ctx.hardLimitMs;
            lctx.softLimitMs = ctx.softLimitMs;
            lctx.nodes = 0;
            lctx.selDepth = 0;
            lctx.rootHistoryLen = (int)root.gameKeys.size();
            lctx.keyStack[0] = root.key;

            while (!stopFlag.load(std::memory_order_relaxed)) {
              int i = next.fetch_add(1);
              if (i >= (int)moves.size()) break;
              Move m = moves[i];

              Undo u;
              root.make(m, u);
              lctx.keyStack[1] = root.key;
              int sc = -negamax(root, -INF, INF, depth - 1, 1, true, m, lctx);
              root.unmake(m, u);

              int cur = bestSc.load();
              while (sc > cur && !bestSc.compare_exchange_weak(cur, sc)) {}
              if (sc == bestSc.load()) bestMv.store((uint32_t)m);
            }
            totalNodes.fetch_add(lctx.nodes);
          };

          // Thread 0 runs in this thread, others launched.
          for (int t = 1; t < nThreads; t++) rootWorkers.emplace_back([&, t]{ worker_fn(t); });
          worker_fn(0);
          for (auto& th : rootWorkers) if (th.joinable()) th.join();

          ctx.nodes += (uint64_t)totalNodes.load();
          score = bestSc.load();
          ctx.stack[0].pvMove = (Move)bestMv.load();
        }
      }

      if (stopFlag.load()) break;

      bestScore = score;

      // Prefer the root move from the last search (avoids TT-collision weirdness)
      best = ctx.stack[0].pvMove;
      if (!best) {
        TTEntry tte;
        if (tt.probe(pos.key, tte) && tte.bestMove) best = (Move)tte.bestMove;
      }

      if (best && !is_legal(pos, best)) best = 0;

      std::string pv = best ? build_pv(pos, *this, best, 32) : std::string();
      print_info(1, bestScore, pv);
    }

    // Track PV stability across iterations (used for soft-stop)
    if (best != 0 && best == prevIterBest && std::abs(bestScore - prevIterScore) <= 15) stableCount++;
    else stableCount = 0;
    prevIterBest = best;
    prevIterScore = bestScore;

    // Update root ordering hints: lightly decay old hints and keep the current best.
    // This helps fixed-time stability (keeps good root moves near the top next iteration).
    if (!rootScoreHint.empty()) {
      for (auto& kv : rootScoreHint) kv.second = (kv.second * 3) / 4;
    }
    if (best) rootScoreHint[(uint32_t)best] = bestScore;

    // Current elapsed time for hard/soft checks
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx.start).count();

    // Decay heuristic tables occasionally (keeps them responsive across a game)
    if ((depth & 1) == 0) heurByThread[0].decay();

    // Hard stop if time up
    if (ctx.hardLimitMs > 0 && ms >= ctx.hardLimitMs) {
      stopFlag.store(true);
      break;
    }

    // Soft stop: if PV is stable, stop early (keeps bestmove coherent and avoids late aspiration blowups).
    if (ctx.softLimitMs > 0 && ms >= ctx.softLimitMs && stableCount >= 2 && best != 0) {
      break;
    }
  }

  // Safety: verify we output a legal bestmove.
  if (best && !is_legal(pos, best)) best = 0;

  if (!best) {
    // fall back to first legal move
    MoveList ml; pos.gen_pseudo(ml);
    Color us = pos.stm;
    for (int i=0;i<ml.size;i++){
      Move m = ml.moves[i];
      Undo u;
      pos.make(m,u);
      bool legal = !pos.is_attacked(pos.kingSq[us], !us);
      pos.unmake(m,u);
      if (legal){ best = m; break; }
    }
  }

  // Stop and join helper threads (if any). Reset stopFlag for the next search.
  if (!workers.empty()) {
    stopFlag.store(true, std::memory_order_relaxed);
    for (auto& th : workers) if (th.joinable()) th.join();
    stopFlag.store(false, std::memory_order_relaxed);
  } else {
    // Ensure clean state
    stopFlag.store(false, std::memory_order_relaxed);
  }
  return best;
}
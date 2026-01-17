#include "search.h"
#include "eval.h"
#include <iostream>
#include <algorithm>
#include <chrono>

static constexpr int INF = 32000;
static constexpr int MATE = 30000;

static inline int now_ms() {
  using namespace std::chrono;
  return (int)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t zobrist_key_slow(const Position& pos); // in tt.cpp

static int score_capture(Piece cap, Piece p) {
  static int v[6] = {100,320,330,500,900,20000};
  return 100000 + v[cap] * 16 - v[p];
}

static int piece_value(Piece p){
  static int v[6] = {100,320,330,500,900,20000};
  return v[p];
}

static bool is_legal_after_make(Position& p, Color sideJustMoved) {
  int ksq = p.kingSq[sideJustMoved];
  return !p.is_attacked(ksq, p.stm);
}

static void gen_captures_only(const Position& pos, MoveList& out) {
  MoveList all;
  pos.gen_pseudo(all);
  out.size = 0;
  for (int i=0;i<all.size;i++){
    Move m = all.moves[i];
    if (m_cap(m) != NO_PIECE || (m_flags(m) & MF_EP)) out.push(m);
  }
}

// very small ordering: TT move, captures MVV-LVA, promos, then rest
static void order_moves(const Position& pos, MoveList& ml, Move ttMove) {
  auto score = [&](Move m)->int{
    if (m == ttMove) return 1'000'000'000;
    int s = 0;
    if (m_flags(m) & MF_PROMO) s += 900000;
    if (m_cap(m) != NO_PIECE) s += score_capture(m_cap(m), m_piece(m));
    if (m_flags(m) & MF_EP) s += 110000; // treat EP as capture
    return s;
  };
  // insertion sort (ml size is small); later swap to partial sort for speed
  for (int i=1;i<ml.size;i++){
    Move key = ml.moves[i];
    int sk = score(key);
    int j=i-1;
    while (j>=0 && score(ml.moves[j]) < sk){
      ml.moves[j+1] = ml.moves[j];
      --j;
    }
    ml.moves[j+1] = key;
  }
}

struct SearchCtx {
  Searcher* S;
  int start_ms;
  int stop_ms;   // hard time stop
  int soft_ms;   // try to stop after completing depth
  uint64_t nodes = 0;
  Move bestMove = 0;
};

static inline bool time_up(SearchCtx& ctx) {
  if (ctx.S->stopFlag.load(std::memory_order_relaxed)) return true;
  if ((ctx.nodes & 8191ULL) == 0ULL) { // check every 8192 nodes
    if (now_ms() >= ctx.stop_ms) return true;
  }
  return false;
}

static int qsearch(Position& pos, SearchCtx& ctx, int alpha, int beta) {
  if (time_up(ctx)) return eval(pos);

  int stand = eval(pos);
  if (stand >= beta) return stand;
  if (stand > alpha) alpha = stand;

  MoveList caps;
  gen_captures_only(pos, caps);

  // no TT ordering here (minimal)
  for (int i=0;i<caps.size;i++){
    Move m = caps.moves[i];
    Undo u;
    Color us = pos.stm;
    pos.make(m, u);
    if (!is_legal_after_make(pos, us)) { pos.unmake(m, u); continue; }
    ctx.nodes++;

    int score = -qsearch(pos, ctx, -beta, -alpha);
    pos.unmake(m, u);

    if (score >= beta) return score;
    if (score > alpha) alpha = score;
  }
  return alpha;
}

static int negamax(Position& pos, SearchCtx& ctx, int depth, int alpha, int beta) {
  if (time_up(ctx)) return eval(pos);

  if (depth <= 0) return qsearch(pos, ctx, alpha, beta);

  // TT probe
  uint64_t key = zobrist_key_slow(pos);
  TTEntry e;
  Move ttMove = 0;
  if (ctx.S->tt.probe(key, e)) {
    ttMove = e.bestMove;
    if (e.depth >= depth) {
      int score = ctx.S->tt.unpack_score(e.score, depth);
      if (e.flag == TT_EXACT) return score;
      if (e.flag == TT_ALPHA && score <= alpha) return score;
      if (e.flag == TT_BETA  && score >= beta)  return score;
    }
  }

  MoveList ml;
  pos.gen_pseudo(ml);
  order_moves(pos, ml, ttMove);

  int bestScore = -INF;
  Move bestMove = 0;
  int alphaOrig = alpha;

  Color us = pos.stm;

  for (int i=0;i<ml.size;i++){
    Move m = ml.moves[i];
    Undo u;
    pos.make(m, u);
    if (!is_legal_after_make(pos, us)) { pos.unmake(m, u); continue; }
    ctx.nodes++;

    int score = -negamax(pos, ctx, depth-1, -beta, -alpha);
    pos.unmake(m, u);

    if (score > bestScore) { bestScore = score; bestMove = m; }
    if (score > alpha) { alpha = score; }
    if (alpha >= beta) break;
  }

  // store TT
  uint8_t flag = TT_EXACT;
  if (bestScore <= alphaOrig) flag = TT_ALPHA;
  else if (bestScore >= beta) flag = TT_BETA;
  ctx.S->tt.store(key, depth, ctx.S->tt.pack_score(bestScore, depth), flag, bestMove);

  return bestScore;
}

// UCI move parser: generate and match from/to/promo (guarantees legality by filtering)
Move parse_uci_move(Position& pos, const std::string& uci) {
  if (uci.size() < 4) return 0;
  auto sq = [](char file, char rank)->int{
    int f = file - 'a';
    int r = rank - '1';
    if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
    return r*8 + f;
  };
  int from = sq(uci[0], uci[1]);
  int to   = sq(uci[2], uci[3]);
  if (from < 0 || to < 0) return 0;

  Piece promo = NO_PIECE;
  if (uci.size() >= 5) {
    char pc = uci[4];
    if (pc=='n') promo=KNIGHT;
    else if (pc=='b') promo=BISHOP;
    else if (pc=='r') promo=ROOK;
    else promo=QUEEN;
  }

  MoveList ml;
  pos.gen_pseudo(ml);
  for (int i=0;i<ml.size;i++){
    Move m = ml.moves[i];
    if (m_from(m) != from || m_to(m) != to) continue;
    if (m_flags(m) & MF_PROMO) {
      if (promo == NO_PIECE) continue;
      if (m_promo(m) != promo) continue;
    } else {
      if (promo != NO_PIECE) continue;
    }
    // legality check by make/unmake
    Undo u;
    Color us = pos.stm;
    pos.make(m, u);
    bool ok = is_legal_after_make(pos, us);
    pos.unmake(m, u);
    if (ok) return m;
  }
  return 0;
}

void Searcher::clear() {
  tt.clear();
}

void Searcher::stop() {
  stopFlag.store(true, std::memory_order_relaxed);
}

void Searcher::tt_resize_mb(int mb) {
  tt.resize_mb(mb);
}

static int compute_time_limit_ms(const Position& pos, const GoLimits& lim) {
  // simple, safe time manager:
  // allocate about time/30 + 0.8*inc, with caps
  int time = (pos.stm == WHITE) ? lim.wtime_ms : lim.btime_ms;
  int inc  = (pos.stm == WHITE) ? lim.winc_ms  : lim.binc_ms;
  if (time <= 0) return 50; // fallback

  int mtg = lim.movestogo > 0 ? lim.movestogo : 30;
  int slice = time / mtg;
  int alloc = slice + (inc * 8) / 10;

  // safety caps
  int maxAlloc = std::max(50, time / 4);
  if (alloc > maxAlloc) alloc = maxAlloc;

  // safety margin
  alloc = std::max(20, alloc - 10);
  return alloc;
}

Move Searcher::go(Position& pos, const GoLimits& lim) {
  stopFlag.store(false, std::memory_order_relaxed);

  const int start = now_ms();
  int timeLimit = 0;
  if (lim.depth > 0) timeLimit = 24*60*60*1000;
  else if (lim.movetime_ms > 0) timeLimit = std::max(5, lim.movetime_ms - 5);
  else timeLimit = compute_time_limit_ms(pos, lim);
  int stopAt = start + timeLimit;

  SearchCtx ctx;
  ctx.S = this;
  ctx.start_ms = start;
  ctx.stop_ms = stopAt;
  ctx.soft_ms = stopAt;

  Move best = 0;
  int bestScore = -INF;

  // iterative deepening
  int maxDepth = (lim.depth > 0) ? lim.depth : 7;

  for (int depth=1; depth<=maxDepth; depth++) {
    if (stopFlag.load(std::memory_order_relaxed)) break;
    if (now_ms() >= stopAt) break;

    int alpha = -INF, beta = INF;
    int score = negamax(pos, ctx, depth, alpha, beta);

    if (stopFlag.load(std::memory_order_relaxed) || now_ms() >= stopAt) break;

    // pull PV move from TT if stored, otherwise keep last best
    TTEntry e;
    if (tt.probe(zobrist_key_slow(pos), e) && e.bestMove) best = e.bestMove;

    bestScore = score;

    // info line (minimal)
    std::cout << "info depth " << depth
              << " nodes " << ctx.nodes
              << " score cp " << bestScore
              << "\n";
    std::cout.flush();
  }

  // if TT didn’t give a move, pick first legal
  if (!best) {
    MoveList ml;
    pos.gen_pseudo(ml);
    for (int i=0;i<ml.size;i++){
      Undo u;
      Color us = pos.stm;
      pos.make(ml.moves[i], u);
      bool ok = is_legal_after_make(pos, us);
      pos.unmake(ml.moves[i], u);
      if (ok) { best = ml.moves[i]; break; }
    }
  }

  if (!best) best = make_move(0,0,PAWN,NO_PIECE,NO_PIECE,MF_NONE); // should never happen
  return best;
}

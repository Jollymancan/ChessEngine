#include "perft.h"
#include <thread>
#include <vector>

uint64_t perft(Position& pos, int depth) {
  if (depth == 0) return 1ULL;

  MoveList ml;
  pos.gen_pseudo(ml);

  uint64_t nodes = 0;
  Undo u;

  for (int i=0;i<ml.size;i++) {
    Move m = ml.moves[i];
    pos.make(m, u);

    Color justMoved = !pos.stm;
    int ksq = pos.kingSq[justMoved];
    if (!pos.is_attacked(ksq, pos.stm)) {
      nodes += perft(pos, depth-1);
    }

    pos.unmake(m, u);
  }
  return nodes;
}

uint64_t perft_root_mt(const Position& root, int depth, int threads) {
  if (threads <= 1) {
    Position p = root;
    return perft(p, depth);
  }

  Position tmp = root;
  MoveList ml;
  tmp.gen_pseudo(ml);

  std::vector<std::thread> pool;
  std::vector<uint64_t> partial(threads, 0ULL);

  auto worker = [&](int tid) {
    Position p = root;
    Undo u;
    uint64_t sum = 0;
    for (int i=tid; i<ml.size; i+=threads) {
      Move m = ml.moves[i];
      p.make(m, u);
      Color justMoved = !p.stm;
      int ksq = p.kingSq[justMoved];
      if (!p.is_attacked(ksq, p.stm)) {
        sum += perft(p, depth-1);
      }
      p.unmake(m, u);
    }
    partial[tid] = sum;
  };

  pool.reserve(threads);
  for (int t=0;t<threads;t++) pool.emplace_back(worker, t);
  for (auto& th : pool) th.join();

  uint64_t total = 0;
  for (auto v : partial) total += v;
  return total;
}

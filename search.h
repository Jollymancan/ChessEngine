#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include "position.h"
#include "tt.h"

struct GoLimits {
  int wtime_ms = 0, btime_ms = 0;
  int winc_ms = 0, binc_ms = 0;
  int movestogo = 0;
  int depth = 0; // if >0, fixed depth search
  int movetime_ms = 0; // <-- ADD THIS
};

Move parse_uci_move(Position& pos, const std::string& uci);

struct Searcher {
  std::atomic<bool> stopFlag{false};
  TT tt;

  void clear();
  void stop();
  void tt_resize_mb(int mb);

  Move go(Position& pos, const GoLimits& lim);
};

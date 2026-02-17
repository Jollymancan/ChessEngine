#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
#include "position.h"
#include "tt.h"
#include "polyglot_book.h"

struct GoLimits {
  int wtime_ms = 0, btime_ms = 0;
  int winc_ms = 0, binc_ms = 0;
  int movestogo = 0;
  int depth = 0;       // if >0, fixed depth search
  int movetime_ms = 0; // if >0, fixed time search
};

// Shared ply limit across the engine.
static constexpr int SEARCH_MAX_PLY = 128;

Move parse_uci_move(Position& pos, const std::string& uci);

struct Searcher {
  std::atomic<bool> stopFlag{false};
  TT tt;

  static constexpr int MAX_PLY = SEARCH_MAX_PLY;

  struct Heuristics {
    Move killers[MAX_PLY][2]{};
    int history[2][64][64]{};              // [side][from][to]
    Move countermove[2][64][64]{};         // [side][prev_from][prev_to]
    int contHist[2][6][64][6][64]{};       // [side][prevPiece][prevTo][piece][to]
    int captureHist[6][64][6]{};          // [attackerPiece][to][capturedPiece]

    void clear();
    void decay();
  };

  Searcher();

  // Threading (Lazy SMP): each thread has its own heuristics tables.
  int threads = 1;
  std::vector<Heuristics> heurByThread{1};

  void set_threads(int n);

  // Options
  int maxDepth = 0;
  // Fixed-time / clock safety
  // Subtracted from UCI movetime (and also used as a safety margin for clock-based searches).
  int moveOverheadMs = 50;
  bool useSyzygy = true;
  std::string syzygyPath;
  int multiPV = 1;
// Opening book (Polyglot .bin)
bool useBook = true;
bool bookWeightedRandom = true;
int bookMinWeight = 1;
int bookMaxPly = 20; // use book only if game ply < bookMaxPly
PolyglotBook book;
  // Last book probe info (for UI / logging)
  mutable uint16_t lastBookWeight = 0;
  mutable int lastBookCandidates = 0;


  void clear();
  void stop();
  void tt_resize_mb(int mb);

  // UCI options
void set_syzygy_path(const std::string& path);

// UCI book options
void set_book_file(const std::string& path);
void set_use_book(bool v) { useBook = v; }
void set_book_weighted_random(bool v) { bookWeightedRandom = v; }
void set_book_min_weight(int w) { bookMinWeight = w; }
void set_book_max_ply(int p) { bookMaxPly = p; }

// Probe book at root. Returns a legal move (as engine Move) if found.
Move probe_book(Position& pos) const;


  Move go(Position& pos, const GoLimits& lim);
};

#include "uci.h"
#include "fen.h"
#include "search.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <optional>
#include "position.h"

static std::string move_to_uci(Move m) {
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
      default: pc = 'q';
    }
    s.push_back(pc);
  }
  return s;
}

static bool parse_position_cmd(Position& pos, const std::string& line) {
  std::istringstream iss(line);
  std::string token;
  iss >> token; // "position"
  if (!(iss >> token)) return false;

  if (token == "startpos") {
    const std::string startpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    if (!load_fen(pos, startpos)) return false;
  } else if (token == "fen") {
    std::string fen, w;
    int fields = 0;
    while (iss >> w) {
      if (w == "moves") break;
      fen += (fields++ ? " " : "") + w;
      if (fields >= 6) {
      }
    }
    if (!load_fen(pos, fen)) return false;
    if (w != "moves") return true; // no moves
  } else {
    return false;
  }

  // apply moves if present
  std::string movesTok;
  if (!(iss >> movesTok)) return true;
  if (movesTok != "moves") return true;

  std::string ms;
  while (iss >> ms) {
    Move m = parse_uci_move(pos, ms);
    if (m == 0) return false;
    Undo u;
    pos.make(m, u);
    // no need to store undo history for GUI position reconstruction
  }
  return true;
}

void uci_loop(Position& pos) {
  Searcher searcher;
  searcher.tt_resize_mb(64); // safe default; change later

  std::atomic<bool> searching{false};
  std::thread searchThread;

  auto join_if_needed = [&](){
    if (searchThread.joinable()) searchThread.join();
  };

auto stop_search = [&](){
  searcher.stop();        // safe even if not searching
  join_if_needed();       // ALWAYS join if joinable
  searching.store(false);
};

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "uci") {
      std::cout << "id name Chessy\n";
      std::cout << "id author prani\n";
      std::cout << "uciok\n";
      std::cout.flush();
    } else if (line == "isready") {
      std::cout << "readyok\n";
      std::cout.flush();
    } else if (line.rfind("setoption", 0) == 0) {
    } else if (line == "ucinewgame") {
      stop_search();
      searcher.clear();
    } else if (line.rfind("position", 0) == 0) {
      stop_search();
      if (!parse_position_cmd(pos, line)) {
        // ignore malformed position
      }
    } else if (line.rfind("go", 0) == 0) {
      stop_search();
      join_if_needed();

      GoLimits lim{};
      lim.depth = 0; // 0 implies time-based
      lim.movestogo = 0;

      // parse go params
      std::istringstream iss(line);
      std::string tok;
      iss >> tok; // go
      while (iss >> tok) {
        if (tok == "wtime") iss >> lim.wtime_ms;
        else if (tok == "btime") iss >> lim.btime_ms;
        else if (tok == "winc") iss >> lim.winc_ms;
        else if (tok == "binc") iss >> lim.binc_ms;
        else if (tok == "movestogo") iss >> lim.movestogo;
        else if (tok == "depth") iss >> lim.depth;
        else if (tok == "movetime") iss >> lim.movetime_ms;
        
      }

      // launch async search so "stop" works
      searching.store(true);
      Position rootCopy = pos;
      searchThread = std::thread([&, rootCopy, lim]() mutable {
        Move best = searcher.go(rootCopy, lim);
        std::cout << "bestmove " << move_to_uci(best) << "\n";
        std::cout.flush();
        searching.store(false);
      });

    } else if (line == "stop") {
      stop_search();
    } else if (line == "quit") {
      stop_search();
      break;
    }
  }

  stop_search();
}

#include "uci.h"
#include "fen.h"
#include "search.h"
#include "params.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <memory>
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
    pos.push_game_key();
    // no need to store undo history for GUI position reconstruction
  }
  return true;
}

void uci_loop(Position& pos) {
  auto searcher = std::make_unique<Searcher>();
  searcher->tt_resize_mb(64); // safe default; change later

  std::atomic<bool> searching{false};
  std::thread searchThread;

  auto join_if_needed = [&](){
    if (searchThread.joinable()) searchThread.join();
  };

auto stop_search = [&](){
  searcher->stop();        // safe even if not searching
  join_if_needed();       // ALWAYS join if joinable
  searching.store(false);
};

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "uci") {
      std::cout << "id name Chessy\n";
      std::cout << "id author prani\n";
      std::cout << "option name Hash type spin default 64 min 1 max 2048\n";
      std::cout << "option name Threads type spin default 1 min 1 max 64\n";
      std::cout << "option name MoveOverhead type spin default 50 min 0 max 500\n";
      std::cout << "option name UseSyzygy type check default true\n";
      std::cout << "option name SyzygyPath type string default \n";
      std::cout << "option name OwnBook type check default true\n";
      std::cout << "option name BookFile type string default \n";
      std::cout << "option name BookRandom type check default true\n";
      std::cout << "option name BookMinWeight type spin default 1 min 0 max 65535\n";
      std::cout << "option name BookMaxPly type spin default 20 min 0 max 200\n";
      std::cout << "option name MultiPV type spin default 1 min 1 max 10\n";
      std::cout << "option name ParamFile type string default \n";
      std::cout << "uciok\n";
      std::cout.flush();
    } else if (line == "isready") {
      std::cout << "readyok\n";
      std::cout.flush();
        } else if (line.rfind("setoption", 0) == 0) {
      // setoption name <Name> value <Value>
      std::istringstream iss(line);
      std::string tok;
      iss >> tok; // setoption
      std::string name, value;
      iss >> tok; // name
      if (tok != "name") continue;
      while (iss >> tok) {
        if (tok == "value") break;
        if (!name.empty()) name.push_back(' ');
        name += tok;
      }
      std::getline(iss, value);
      // trim leading spaces
      while (!value.empty() && value[0] == ' ') value.erase(value.begin());

if (name == "Hash") {
  try {
    int mb = std::stoi(value);
    mb = std::max(1, std::min(2048, mb));
    searcher->tt_resize_mb(mb);
  } catch (...) {}
} else if (name == "MoveOverhead") {
  try {
    int ms = std::stoi(value);
    ms = std::max(0, std::min(500, ms));
    searcher->moveOverheadMs = ms;
  } catch (...) {}
} else if (name == "SyzygyPath") {
  searcher->set_syzygy_path(value);
} else if (name == "Threads") {
  try {
    int n = std::stoi(value);
    n = std::max(1, std::min(64, n));
    searcher->set_threads(n);
  } catch (...) {}
} else if (name == "UseSyzygy") {
  if (value == "false" || value == "0") searcher->useSyzygy = false;
  else searcher->useSyzygy = true;
} else if (name == "OwnBook") {
  if (value == "false" || value == "0") searcher->set_use_book(false);
  else searcher->set_use_book(true);
} else if (name == "BookFile") {
  searcher->set_book_file(value);
} else if (name == "BookRandom") {
  if (value == "false" || value == "0") searcher->set_book_weighted_random(false);
  else searcher->set_book_weighted_random(true);
} else if (name == "BookMinWeight") {
  try {
    int w = std::stoi(value);
    if (w < 0) w = 0;
    if (w > 65535) w = 65535;
    searcher->set_book_min_weight(w);
  } catch (...) {}
} else if (name == "BookMaxPly") {
  try {
    int p = std::stoi(value);
    if (p < 0) p = 0;
    if (p > 200) p = 200;
    searcher->set_book_max_ply(p);
  } catch (...) {}
} else if (name == "MultiPV") {
  try {
    int n = std::stoi(value);
    n = std::max(1, std::min(10, n));
    searcher->multiPV = n;
  } catch (...) {}
} else if (name == "ParamFile") {
  // Load runtime parameters for tuning. Unknown keys are ignored.
  (void)load_params_file(value);
}
    } else if (line == "ucinewgame") {
      stop_search();
      searcher->clear();
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

      // Opening book (Polyglot) at root: return immediately if found.
      // Determine current game ply from FEN counters.
      int gamePly = (int)(pos.fullmoveNumber - 1) * 2 + (pos.stm == BLACK ? 1 : 0);
      if (searcher->useBook && searcher->book.loaded() && gamePly < searcher->bookMaxPly) {
        Position tmp = pos;
        Move bm = searcher->probe_book(tmp);
        if (bm) {
          std::cout << "info string book move " << move_to_uci(bm)
                    << " weight " << searcher->lastBookWeight
                    << " candidates " << searcher->lastBookCandidates
                    << "\n";
          if (bm) std::cout << "bestmove " << move_to_uci(bm) << "\n";
          else std::cout << "bestmove 0000\n";
          std::cout.flush();
          continue;
        }
      }

      // launch async search so "stop" works
      searching.store(true);
      Position rootCopy = pos;
      searchThread = std::thread([&, rootCopy, lim]() mutable {
        Move best = searcher->go(rootCopy, lim);
        if (best) std::cout << "bestmove " << move_to_uci(best) << "\n";
        else std::cout << "bestmove 0000\n";
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

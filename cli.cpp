#include "cli.h"
#include "fen.h"
#include "search.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <memory>

static char piece_char(int code12) {
  // color*6 + piece, empty is 12
  if (code12 == 12) return '.';
  int c = code12 / 6;
  int p = code12 % 6;
  static const char W[6] = {'P','N','B','R','Q','K'};
  static const char B[6] = {'p','n','b','r','q','k'};
  return (c==0) ? W[p] : B[p];
}

static void print_board(const Position& pos) {
  std::cout << "\n  +-----------------+\n";
  for (int r = 7; r >= 0; --r) {
    std::cout << (r+1) << " | ";
    for (int f = 0; f < 8; ++f) {
      int sq = r*8 + f;
      std::cout << piece_char(pos.board[sq]) << ' ';
    }
    std::cout << "|\n";
  }
  std::cout << "  +-----------------+\n";
  std::cout << "    a b c d e f g h\n";
  std::cout << "Side: " << (pos.stm==WHITE ? "white" : "black") << "\n";
  std::cout << "EP: " << (pos.epSq==-1 ? "-" : std::to_string(pos.epSq)) << "\n\n";
}

static bool has_any_legal_move(Position& pos) {
  MoveList ml;
  pos.gen_pseudo(ml);
  for (int i=0;i<ml.size;i++){
    Undo u;
    Color us = pos.stm;
    pos.make(ml.moves[i], u);
    bool ok = !pos.is_attacked(pos.kingSq[us], pos.stm);
    pos.unmake(ml.moves[i], u);
    if (ok) return true;
  }
  return false;
}

void cli_loop(Position& pos) {
  auto searcher = std::make_unique<Searcher>();
  searcher->tt_resize_mb(64);

  const std::string startpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  load_fen(pos, startpos);

  std::cout << "Chessy CLI mode\n";
  std::cout << "Commands: d | new | fen <...> | move <e2e4> | go <ms> | auto <ms> | quit\n";
  print_board(pos);

  std::string line;
  while (true) {
    std::cout << "chessy> ";
    if (!std::getline(std::cin, line)) break;
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd.empty()) continue;

    if (cmd == "quit" || cmd == "exit") {
      break;
    } else if (cmd == "d") {
      print_board(pos);
    } else if (cmd == "new") {
      searcher->clear();
      load_fen(pos, startpos);
      print_board(pos);
    } else if (cmd == "fen") {
      std::string fen, w;
      int fields = 0;
      while (iss >> w) {
        fen += (fields++ ? " " : "") + w;
      }
      if (!load_fen(pos, fen)) std::cout << "Bad FEN\n";
      else print_board(pos);
    } else if (cmd == "move") {
      std::string uci;
      iss >> uci;
      if (uci.empty()) { std::cout << "Usage: move e2e4\n"; continue; }

      Move m = parse_uci_move(pos, uci);
      if (!m) {
        std::cout << "Illegal/unknown move: " << uci << "\n";
        continue;
      }
      Undo u;
      pos.make(m, u);
      print_board(pos);

      if (!has_any_legal_move(pos)) {
        // checkmate or stalemate
        Color sideInTrouble = pos.stm;
        bool inCheck = pos.is_attacked(pos.kingSq[sideInTrouble], !pos.stm);
        std::cout << (inCheck ? "Checkmate.\n" : "Stalemate.\n");
      }
    } else if (cmd == "go") {
      int ms = 200;
      iss >> ms;
      if (ms <= 0) ms = 1;

      GoLimits lim{};
      lim.movetime_ms = ms;

      Move best = searcher->go(pos, lim);
      if (!best) {
        std::cout << "No legal moves.\n";
        continue;
      }

      // apply best move
      Undo u;
      pos.make(best, u);
      std::cout << "engine played: " << line << "\n";
      print_board(pos);

      if (!has_any_legal_move(pos)) {
        Color sideInTrouble = pos.stm;
        bool inCheck = pos.is_attacked(pos.kingSq[sideInTrouble], !pos.stm);
        std::cout << (inCheck ? "Checkmate.\n" : "Stalemate.\n");
      }
    } else if (cmd == "auto") {
      int ms = 200;
      iss >> ms;
      if (ms <= 0) ms = 1;

      GoLimits lim{};
      lim.movetime_ms = ms;

      for (;;) {
        if (!has_any_legal_move(pos)) {
          Color sideInTrouble = pos.stm;
          bool inCheck = pos.is_attacked(pos.kingSq[sideInTrouble], !pos.stm);
          std::cout << (inCheck ? "Checkmate.\n" : "Stalemate.\n");
          break;
        }
        Move best = searcher->go(pos, lim);
        if (!best) { std::cout << "No move.\n"; break; }
        Undo u;
        pos.make(best, u);
        print_board(pos);
      }
    } else {
      std::cout << "Unknown command. Try: d, new, fen, move, go, auto, quit\n";
    }
  }
}
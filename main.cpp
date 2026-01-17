#include "attacks.h"
#include "fen.h"
#include "uci.h"
#include "cli.h"
#include <string>

int main(int argc, char** argv) {
  ATK.init();

  Position pos;
  const std::string startpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  load_fen(pos, startpos);

  // If you run: chessy.exe --cli
  if (argc >= 2 && std::string(argv[1]) == "--cli") {
    cli_loop(pos);
  } else {
    uci_loop(pos);
  }
  return 0;
}

#include "fen.h"
#include "bitboard.h"
#include "zobrist.h"
#include <cctype>

static int sq_from_alg(const std::string& s) {
  if (s.size() != 2) return NO_SQ;
  char file = s[0], rank = s[1];
  if (file < 'a' || file > 'h' || rank < '1' || rank > '8') return NO_SQ;
  int f = file - 'a';
  int r = rank - '1';
  return r*8 + f;
}

static void set_piece(Position& pos, int sq, Color c, Piece p) {
  pos.board[sq] = code(c,p);
  pos.bb[c][p] |= sq_bb(sq);
  if (p == KING) pos.kingSq[c] = sq;
}

bool load_fen(Position& out, const std::string& fen) {
  // Ensure zobrist tables are ready for rebuild_key().
  zobrist_init();
  Position p;
  int i=0;
  int sq = 56; // a8
  while (i < (int)fen.size() && fen[i] != ' ') {
    char c = fen[i++];
    if (c == '/') { sq -= 16; continue; }
    if (c >= '1' && c <= '8') { sq += (c - '0'); continue; }

    Color col = (c >= 'a' && c <= 'z') ? BLACK : WHITE;
    char uc = (char)std::toupper((unsigned char)c);
    Piece pc = NO_PIECE;
    if      (uc=='P') pc=PAWN;
    else if (uc=='N') pc=KNIGHT;
    else if (uc=='B') pc=BISHOP;
    else if (uc=='R') pc=ROOK;
    else if (uc=='Q') pc=QUEEN;
    else if (uc=='K') pc=KING;
    else return false;

    set_piece(p, sq, col, pc);
    sq++;
  }

  if (i >= (int)fen.size() || fen[i] != ' ') return false;
  i++;

  if (fen[i] == 'w') p.stm = WHITE;
  else if (fen[i] == 'b') p.stm = BLACK;
  else return false;
  while (i < (int)fen.size() && fen[i] != ' ') i++;
  if (i >= (int)fen.size()) return false;
  i++;

  p.castling = 0;
  if (fen[i] == '-') {
    i++;
  } else {
    while (i < (int)fen.size() && fen[i] != ' ') {
      switch(fen[i]) {
        case 'K': p.castling |= 1; break;
        case 'Q': p.castling |= 2; break;
        case 'k': p.castling |= 4; break;
        case 'q': p.castling |= 8; break;
      }
      i++;
    }
  }
  if (i >= (int)fen.size() || fen[i] != ' ') return false;
  i++;

  if (fen[i] == '-') {
    p.epSq = NO_SQ;
    i++;
  } else {
    std::string eps;
    eps.push_back(fen[i++]);
    eps.push_back(fen[i++]);
    p.epSq = sq_from_alg(eps);
  }


  // Optional halfmove/fullmove
  while (i < (int)fen.size() && fen[i] == ' ') i++;
  // halfmove clock
  if (i < (int)fen.size()) {
    int hm = 0;
    while (i < (int)fen.size() && std::isdigit((unsigned char)fen[i])) {
      hm = hm*10 + (fen[i]-'0'); i++;
    }
    p.halfmoveClock = (uint16_t)std::min(hm, 1000);
  }
  while (i < (int)fen.size() && fen[i] == ' ') i++;
  if (i < (int)fen.size()) {
    int fm = 0;
    while (i < (int)fen.size() && std::isdigit((unsigned char)fen[i])) {
      fm = fm*10 + (fen[i]-'0'); i++;
    }
    if (fm > 0) p.fullmoveNumber = (uint16_t)std::min(fm, 10000);
  }

  p.rebuild_occ();
  p.rebuild_key();
  p.reset_game_history();
  out = p;
  return true;
}

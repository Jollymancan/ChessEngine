#include "attacks.h"
#include "bitboard.h"

Attacks ATK;

static inline bool on_board(int f, int r) { return f>=0 && f<8 && r>=0 && r<8; }

void Attacks::init() {
  for (int sq=0; sq<64; sq++) {
    int f = file_of(sq), r = rank_of(sq);

    pawn[WHITE][sq] = 0;
    pawn[BLACK][sq] = 0;

    if (on_board(f-1, r+1)) pawn[WHITE][sq] |= sq_bb((r+1)*8 + (f-1));
    if (on_board(f+1, r+1)) pawn[WHITE][sq] |= sq_bb((r+1)*8 + (f+1));
    if (on_board(f-1, r-1)) pawn[BLACK][sq] |= sq_bb((r-1)*8 + (f-1));
    if (on_board(f+1, r-1)) pawn[BLACK][sq] |= sq_bb((r-1)*8 + (f+1));

    const int dfN[8]={-2,-2,-1,-1, 1, 1, 2, 2};
    const int drN[8]={-1, 1,-2, 2,-2, 2,-1, 1};
    U64 k=0;
    for(int i=0;i<8;i++){
      int nf=f+dfN[i], nr=r+drN[i];
      if(on_board(nf,nr)) k |= sq_bb(nr*8+nf);
    }
    knight[sq]=k;

    U64 g=0;
    for(int df=-1; df<=1; df++){
      for(int dr=-1; dr<=1; dr++){
        if(df==0 && dr==0) continue;
        int nf=f+df, nr=r+dr;
        if(on_board(nf,nr)) g |= sq_bb(nr*8+nf);
      }
    }
    king[sq]=g;
  }
}

// baseline ray sliders (correct; later swap to magics/BMI2)
U64 rook_attacks(int sq, U64 occ) {
  U64 a=0;
  int f=file_of(sq), r=rank_of(sq);
  for(int rr=r+1; rr<8; rr++){ int s=rr*8+f; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int rr=r-1; rr>=0; rr--){ int s=rr*8+f; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int ff=f+1; ff<8; ff++){ int s=r*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int ff=f-1; ff>=0; ff--){ int s=r*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  return a;
}

U64 bishop_attacks(int sq, U64 occ) {
  U64 a=0;
  int f=file_of(sq), r=rank_of(sq);
  for(int ff=f+1, rr=r+1; ff<8 && rr<8; ff++,rr++){ int s=rr*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int ff=f-1, rr=r+1; ff>=0 && rr<8; ff--,rr++){ int s=rr*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int ff=f+1, rr=r-1; ff<8 && rr>=0; ff++,rr--){ int s=rr*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  for(int ff=f-1, rr=r-1; ff>=0 && rr>=0; ff--,rr--){ int s=rr*8+ff; a|=sq_bb(s); if(occ & sq_bb(s)) break; }
  return a;
}

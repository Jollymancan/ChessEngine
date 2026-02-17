#include "attacks.h"
#include "bitboard.h"

#include <array>
#include <cstdint>

Attacks ATK;

static inline bool on_board(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

// ----------------------------
// Sliding attacks via magics
// ----------------------------
namespace {

// Magic constants (a1 = 0, little-endian file/rank mapping).
// These are commonly used "good" multipliers; we still validate
// them at startup and will search for a replacement if any collisions occur.
static constexpr std::array<U64, 64> ROOK_MAGICS = {
    0x0a8002c000108020ULL,
    0x006c00049b0002001ULL,
    0x0100200010090040ULL,
    0x2480041000800801ULL,
    0x0280028004000800ULL,
    0x0900410008040022ULL,
    0x0280020001001080ULL,
    0x2880002041000080ULL,
    0x0a000800080400034ULL,
    0x0004808020004000ULL,
    0x2290802004801000ULL,
    0x0411000d00100020ULL,
    0x0402800800040080ULL,
    0x000b000401004208ULL,
    0x2409000100040200ULL,
    0x0001002100004082ULL,
    0x0022878001e24000ULL,
    0x1090810021004010ULL,
    0x0801030040200012ULL,
    0x000500808008001000ULL,
    0x0a08018014000880ULL,
    0x8000808004000200ULL,
    0x0201008080010200ULL,
    0x0801020000441091ULL,
    0x0008000802040005ULL,
    0x1040200040100048ULL,
    0x0001202004020082ULL,
    0x0d14880480100080ULL,
    0x12040280080080ULL,
    0x0100040080020080ULL,
    0x9020010080800200ULL,
    0x0813241200148449ULL,
    0x0491604001800080ULL,
    0x000100401000402001ULL,
    0x4820010021001040ULL,
    0x0400402202000812ULL,
    0x0209009005000802ULL,
    0x0810800601800400ULL,
    0x4301083214000150ULL,
    0x204026458e001401ULL,
    0x0040204000808000ULL,
    0x8001008040010020ULL,
    0x8410820820420010ULL,
    0x01003001000090020ULL,
    0x0804040008008080ULL,
    0x0012000810020004ULL,
    0x1000100200040208ULL,
    0x430000a044020001ULL,
    0x0280009023410300ULL,
    0x0e01000400002240ULL,
    0x000200100401700ULL,
    0x2244100408008080ULL,
    0x00080000400801980ULL,
    0x0002000810040200ULL,
    0x8010100228810400ULL,
    0x2000009044210200ULL,
    0x4080008040102101ULL,
    0x0040002080411d01ULL,
    0x2005524060000901ULL,
    0x0502001008400422ULL,
    0x489a000810200402ULL,
    0x0001004400080a13ULL,
    0x4000011008020084ULL,
    0x0026002114058042ULL,
};

static constexpr std::array<U64, 64> BISHOP_MAGICS = {
    0x0420c80100408202ULL,
    0x1204311202260108ULL,
    0x2008208102030000ULL,
    0x000024081001000caULL,
    0x0488484041002110ULL,
    0x001a080c2c010018ULL,
    0x0020a02a2400084ULL,
    0x0440404400a01000ULL,
    0x0008931041080080ULL,
    0x0002004841080221ULL,
    0x0080460802188000ULL,
    0x4000090401080092ULL,
    0x4000011040a00004ULL,
    0x0020011048040504ULL,
    0x2008008401084000ULL,
    0x000102422a101a02ULL,
    0x2040801082420404ULL,
    0x8104900210440100ULL,
    0x0202101012820109ULL,
    0x0248090401409004ULL,
    0x0044820404a00020ULL,
    0x00040808110100100ULL,
    0x0480a80100882000ULL,
    0x184820208a011010ULL,
    0x0110400206085200ULL,
    0x0001050010104201ULL,
    0x4008480070008010ULL,
    0x8440040018410120ULL,
    0x000041010000104000ULL,
    0x4010004080241000ULL,
    0x0001244082061040ULL,
    0x0051060000288441ULL,
    0x0002215410a05820ULL,
    0x6000941020a0c220ULL,
    0x0000f2080100020201ULL,
    0x8010020081180080ULL,
    0x0940012060060080ULL,
    0x0620008284290800ULL,
    0x0008468100140900ULL,
    0x418400aa01802100ULL,
    0x4000882440015002ULL,
    0x000420220a11081ULL,
    0x0401a26030000804ULL,
    0x0002184208000084ULL,
    0xa430820a0410c201ULL,
    0x0640053805080180ULL,
    0x4a04010a44100601ULL,
    0x00010014901001021ULL,
    0x0422411031300100ULL,
    0x0824222110280000ULL,
    0x8800020a0b340300ULL,
    0x00a8000441109088ULL,
    0x0404000861010208ULL,
    0x0040112002042200ULL,
    0x02141006480b00a0ULL,
    0x2210108081004411ULL,
    0x2010804070100803ULL,
    0x7a0011010090ac31ULL,
    0x0018005100880400ULL,
    0x8010001081084805ULL,
    0x400200021202020aULL,
    0x04100342100a0221ULL,
    0x0404408801010204ULL,
    0x6360041408104012ULL,
};

static U64 rook_mask[64];
static U64 bishop_mask[64];
static U64 rook_magic[64];
static U64 bishop_magic[64];
static int rook_shift[64];
static int bishop_shift[64];

// Max table sizes: rook <= 2^12, bishop <= 2^9
static U64 rook_table[64][4096];
static U64 bishop_table[64][512];

static bool magics_ready = false;

static U64 rook_attacks_slow(int sq, U64 occ) {
  U64 a = 0;
  int f = file_of(sq), r = rank_of(sq);
  for (int rr = r + 1; rr < 8; rr++) {
    int s = rr * 8 + f;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int rr = r - 1; rr >= 0; rr--) {
    int s = rr * 8 + f;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int ff = f + 1; ff < 8; ff++) {
    int s = r * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int ff = f - 1; ff >= 0; ff--) {
    int s = r * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  return a;
}

static U64 bishop_attacks_slow(int sq, U64 occ) {
  U64 a = 0;
  int f = file_of(sq), r = rank_of(sq);
  for (int ff = f + 1, rr = r + 1; ff < 8 && rr < 8; ff++, rr++) {
    int s = rr * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int ff = f - 1, rr = r + 1; ff >= 0 && rr < 8; ff--, rr++) {
    int s = rr * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int ff = f + 1, rr = r - 1; ff < 8 && rr >= 0; ff++, rr--) {
    int s = rr * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  for (int ff = f - 1, rr = r - 1; ff >= 0 && rr >= 0; ff--, rr--) {
    int s = rr * 8 + ff;
    a |= sq_bb(s);
    if (occ & sq_bb(s)) break;
  }
  return a;
}

static U64 rook_relevant_mask(int sq) {
  U64 m = 0;
  int f = file_of(sq), r = rank_of(sq);
  for (int rr = r + 1; rr <= 6; rr++) m |= sq_bb(rr * 8 + f);
  for (int rr = r - 1; rr >= 1; rr--) m |= sq_bb(rr * 8 + f);
  for (int ff = f + 1; ff <= 6; ff++) m |= sq_bb(r * 8 + ff);
  for (int ff = f - 1; ff >= 1; ff--) m |= sq_bb(r * 8 + ff);
  return m;
}

static U64 bishop_relevant_mask(int sq) {
  U64 m = 0;
  int f = file_of(sq), r = rank_of(sq);
  for (int ff = f + 1, rr = r + 1; ff <= 6 && rr <= 6; ff++, rr++) m |= sq_bb(rr * 8 + ff);
  for (int ff = f - 1, rr = r + 1; ff >= 1 && rr <= 6; ff--, rr++) m |= sq_bb(rr * 8 + ff);
  for (int ff = f + 1, rr = r - 1; ff <= 6 && rr >= 1; ff++, rr--) m |= sq_bb(rr * 8 + ff);
  for (int ff = f - 1, rr = r - 1; ff >= 1 && rr >= 1; ff--, rr--) m |= sq_bb(rr * 8 + ff);
  return m;
}

static inline U64 splitmix64(U64& x) {
  x += 0x9e3779b97f4a7c15ULL;
  U64 z = x;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static inline U64 random_u64(U64& s) { return splitmix64(s); }

static inline U64 random_u64_fewbits(U64& s) {
  return random_u64(s) & random_u64(s) & random_u64(s);
}

static bool build_table_for_square(bool rook, int sq, U64 magic) {
  const U64 mask = rook ? rook_mask[sq] : bishop_mask[sq];
  const int shift = rook ? rook_shift[sq] : bishop_shift[sq];
  const int bits = 64 - shift;
  const int size = 1 << bits;

  // Gather squares in the mask
  std::array<int, 12> squares{};
  int n = 0;
  U64 tmp = mask;
  while (tmp) squares[n++] = pop_lsb(tmp);

  // Used map to detect collisions
  // We can't use a sentinel that could equal a real attack (0 is valid), so use a filled flag.
  if (rook) {
    std::array<U64, 4096> local{};
    std::array<uint8_t, 4096> filled{};

    for (int idx = 0; idx < size; idx++) {
      U64 occ = 0;
      for (int b = 0; b < n; b++) {
        if (idx & (1 << b)) occ |= sq_bb(squares[b]);
      }
      U64 att = rook_attacks_slow(sq, occ);
      U64 key = ((occ * magic) >> shift);
      if (filled[key] && local[key] != att) return false;
      filled[key] = 1;
      local[key] = att;
    }

    // Copy into global
    for (int i = 0; i < size; i++) rook_table[sq][i] = local[i];
    return true;
  } else {
    std::array<U64, 512> local{};
    std::array<uint8_t, 512> filled{};

    for (int idx = 0; idx < size; idx++) {
      U64 occ = 0;
      for (int b = 0; b < n; b++) {
        if (idx & (1 << b)) occ |= sq_bb(squares[b]);
      }
      U64 att = bishop_attacks_slow(sq, occ);
      U64 key = ((occ * magic) >> shift);
      if (filled[key] && local[key] != att) return false;
      filled[key] = 1;
      local[key] = att;
    }

    for (int i = 0; i < size; i++) bishop_table[sq][i] = local[i];
    return true;
  }
}

static U64 find_magic_for_square(bool rook, int sq, U64 seed) {
  const U64 mask = rook ? rook_mask[sq] : bishop_mask[sq];
  const int shift = rook ? rook_shift[sq] : bishop_shift[sq];
  const int bits = 64 - shift;
  const int size = 1 << bits;

  // Precompute occupancies + attacks
  std::array<int, 12> squares{};
  int n = 0;
  U64 tmp = mask;
  while (tmp) squares[n++] = pop_lsb(tmp);

  std::array<U64, 4096> occs{};
  std::array<U64, 4096> atts{};
  for (int idx = 0; idx < size; idx++) {
    U64 occ = 0;
    for (int b = 0; b < n; b++) {
      if (idx & (1 << b)) occ |= sq_bb(squares[b]);
    }
    occs[idx] = occ;
    atts[idx] = rook ? rook_attacks_slow(sq, occ) : bishop_attacks_slow(sq, occ);
  }

  U64 state = seed ^ (U64)sq * 0x9e3779b97f4a7c15ULL;

  // Try random candidates until we find a collision-free mapping.
  // This is startup-only.
  for (int iter = 0; iter < 500000; iter++) {
    U64 magic = random_u64_fewbits(state);

    // Quick heuristic from chessprogramming: reject magics that don't spread high bits well.
    if (popcount64((mask * magic) & 0xFF00000000000000ULL) < 6) continue;

    if (rook) {
      std::array<U64, 4096> used{};
      std::array<uint8_t, 4096> filled{};

      bool ok = true;
      for (int i = 0; i < size; i++) {
        U64 key = (occs[i] * magic) >> shift;
        if (filled[key] && used[key] != atts[i]) { ok = false; break; }
        filled[key] = 1;
        used[key] = atts[i];
      }
      if (ok) return magic;
    } else {
      std::array<U64, 512> used{};
      std::array<uint8_t, 512> filled{};

      bool ok = true;
      for (int i = 0; i < size; i++) {
        U64 key = (occs[i] * magic) >> shift;
        if (filled[key] && used[key] != atts[i]) { ok = false; break; }
        filled[key] = 1;
        used[key] = atts[i];
      }
      if (ok) return magic;
    }
  }

  // Fallback (very unlikely): return the provided magic even if it collides,
  // but then we'll skip magics and use slow attacks at runtime for that square.
  return 0ULL;
}

static void init_magics() {
  if (magics_ready) return;

  // Masks + shifts based on the standard relevant-occupancy definition.
  for (int sq = 0; sq < 64; sq++) {
    rook_mask[sq] = rook_relevant_mask(sq);
    bishop_mask[sq] = bishop_relevant_mask(sq);

    int rb = popcount64(rook_mask[sq]);
    int bb = popcount64(bishop_mask[sq]);

    rook_shift[sq] = 64 - rb;
    bishop_shift[sq] = 64 - bb;

    rook_magic[sq] = ROOK_MAGICS[sq];
    bishop_magic[sq] = BISHOP_MAGICS[sq];
  }

  // Build rook + bishop tables.
  // Validate provided magics; if any fails, search for a replacement at startup.
  for (int sq = 0; sq < 64; sq++) {
    if (!build_table_for_square(true, sq, rook_magic[sq])) {
      U64 m = find_magic_for_square(true, sq, 0xC0FFEE123456789ULL);
      if (m) {
        rook_magic[sq] = m;
        (void)build_table_for_square(true, sq, rook_magic[sq]);
      } else {
        // Mark as unusable; rook_attacks will fall back to slow.
        rook_magic[sq] = 0ULL;
      }
    }

    if (!build_table_for_square(false, sq, bishop_magic[sq])) {
      U64 m = find_magic_for_square(false, sq, 0xBADF00DCAFEBEEFULL);
      if (m) {
        bishop_magic[sq] = m;
        (void)build_table_for_square(false, sq, bishop_magic[sq]);
      } else {
        bishop_magic[sq] = 0ULL;
      }
    }
  }

  magics_ready = true;
}

} // namespace

void Attacks::init() {
  for (int sq = 0; sq < 64; sq++) {
    int f = file_of(sq), r = rank_of(sq);

    pawn[WHITE][sq] = 0;
    pawn[BLACK][sq] = 0;

    if (on_board(f - 1, r + 1)) pawn[WHITE][sq] |= sq_bb((r + 1) * 8 + (f - 1));
    if (on_board(f + 1, r + 1)) pawn[WHITE][sq] |= sq_bb((r + 1) * 8 + (f + 1));
    if (on_board(f - 1, r - 1)) pawn[BLACK][sq] |= sq_bb((r - 1) * 8 + (f - 1));
    if (on_board(f + 1, r - 1)) pawn[BLACK][sq] |= sq_bb((r - 1) * 8 + (f + 1));

    const int dfN[8] = {-2, -2, -1, -1, 1, 1, 2, 2};
    const int drN[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
    U64 k = 0;
    for (int i = 0; i < 8; i++) {
      int nf = f + dfN[i], nr = r + drN[i];
      if (on_board(nf, nr)) k |= sq_bb(nr * 8 + nf);
    }
    knight[sq] = k;

    U64 g = 0;
    for (int df = -1; df <= 1; df++) {
      for (int dr = -1; dr <= 1; dr++) {
        if (df == 0 && dr == 0) continue;
        int nf = f + df, nr = r + dr;
        if (on_board(nf, nr)) g |= sq_bb(nr * 8 + nf);
      }
    }
    king[sq] = g;
  }

  init_magics();
}

U64 rook_attacks(int sq, U64 occ) {
  if (!magics_ready || rook_magic[sq] == 0ULL) return rook_attacks_slow(sq, occ);
  U64 x = (occ & rook_mask[sq]) * rook_magic[sq];
  return rook_table[sq][x >> rook_shift[sq]];
}

U64 bishop_attacks(int sq, U64 occ) {
  if (!magics_ready || bishop_magic[sq] == 0ULL) return bishop_attacks_slow(sq, occ);
  U64 x = (occ & bishop_mask[sq]) * bishop_magic[sq];
  return bishop_table[sq][x >> bishop_shift[sq]];
}

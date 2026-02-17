#pragma once

#include <string>

// Runtime-tunable parameters for classical evaluation/search.
// These are used to support automated tuning (SPSA/SPRT) without
// hardcoding everything in source.

struct Params {
  // Aspiration windows (centipawns)
  int asp_base = 18;
  int asp_per_depth = 10;

  // History pruning (quiet moves only). Conservative defaults.
  int hist_prune_min_depth = 8;
  int hist_prune_late_base = 12;
  int hist_prune_late_per_depth = 2;
  int hist_prune_threshold = -2000;

  // Late move reductions tweaks
  int lmr_check_bonus = 1;      // reduce less for checking moves
  int lmr_goodhist_bonus = 1;   // reduce less for good history
  int lmr_badhist_penalty = 1;  // reduce more for bad history

  // King safety (attack units)
  int ks_attacker_bonus = 6;    // per distinct attacker
  int ks_units_n = 6;
  int ks_units_b = 6;
  int ks_units_r = 4;
  int ks_units_q = 10;
  int ks_scale = 1;             // overall scale

  // Threats
  int thr_hanging_minor = 18;
  int thr_hanging_rook  = 28;
  int thr_hanging_queen = 40;
  int thr_pawn_attack_bonus = 8;
};

extern Params g_params;

// Load parameters from a simple text file with lines like: key=value
// Unknown keys are ignored. Returns true on success.
bool load_params_file(const std::string& path);

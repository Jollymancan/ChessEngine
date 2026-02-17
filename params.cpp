#include "params.h"

#include <fstream>
#include <sstream>
#include <cctype>

Params g_params;

static inline std::string trim(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b-1])) b--;
  return s.substr(a, b - a);
}

static bool set_kv(const std::string& k, int v) {
  // Aspiration
  if (k == "asp_base") { g_params.asp_base = v; return true; }
  if (k == "asp_per_depth") { g_params.asp_per_depth = v; return true; }

  // History pruning
  if (k == "hist_prune_min_depth") { g_params.hist_prune_min_depth = v; return true; }
  if (k == "hist_prune_late_base") { g_params.hist_prune_late_base = v; return true; }
  if (k == "hist_prune_late_per_depth") { g_params.hist_prune_late_per_depth = v; return true; }
  if (k == "hist_prune_threshold") { g_params.hist_prune_threshold = v; return true; }

  // LMR tweaks
  if (k == "lmr_check_bonus") { g_params.lmr_check_bonus = v; return true; }
  if (k == "lmr_goodhist_bonus") { g_params.lmr_goodhist_bonus = v; return true; }
  if (k == "lmr_badhist_penalty") { g_params.lmr_badhist_penalty = v; return true; }

  // King safety
  if (k == "ks_attacker_bonus") { g_params.ks_attacker_bonus = v; return true; }
  if (k == "ks_units_n") { g_params.ks_units_n = v; return true; }
  if (k == "ks_units_b") { g_params.ks_units_b = v; return true; }
  if (k == "ks_units_r") { g_params.ks_units_r = v; return true; }
  if (k == "ks_units_q") { g_params.ks_units_q = v; return true; }
  if (k == "ks_scale") { g_params.ks_scale = v; return true; }

  // Threats
  if (k == "thr_hanging_minor") { g_params.thr_hanging_minor = v; return true; }
  if (k == "thr_hanging_rook") { g_params.thr_hanging_rook = v; return true; }
  if (k == "thr_hanging_queen") { g_params.thr_hanging_queen = v; return true; }
  if (k == "thr_pawn_attack_bonus") { g_params.thr_pawn_attack_bonus = v; return true; }
  return false;
}

bool load_params_file(const std::string& path) {
  std::ifstream in(path);
  if (!in.good()) return false;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq));
    std::string vs = trim(line.substr(eq + 1));
    if (k.empty() || vs.empty()) continue;
    int v = 0;
    try {
      v = std::stoi(vs);
    } catch (...) {
      continue;
    }
    (void)set_kv(k, v);
  }
  return true;
}

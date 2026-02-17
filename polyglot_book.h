#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "position.h"

// True Polyglot (.bin) opening book reader.
// Format: big-endian entries (key, move, weight, learn).
struct PolyglotEntry {
  uint64_t key = 0;
  uint16_t move = 0;
  uint16_t weight = 0;
  uint32_t learn = 0;
};

struct PolyglotHit {
  std::string uci;
  uint16_t weight = 0;
  int candidates = 0;
};

class PolyglotBook {
public:
  bool load(const std::string& path);          // returns false on failure
  void clear();
  bool loaded() const { return !entries.empty(); }
  const std::string& filename() const { return filePath; }
  size_t entry_count() const { return entries.size(); }

  // Probe current position; returns UCI move string if found.
  // If weightedRandom=true, chooses randomly proportional to weights; else picks max weight.
  // minWeight filters entries with small weights (0 = accept all).
  std::optional<PolyglotHit> probe(const Position& pos, bool weightedRandom, int minWeight) const;

private:
  std::string filePath;
  std::vector<PolyglotEntry> entries;

  static uint64_t polyglot_key(const Position& pos);
  static std::string polyglot_move_to_uci(uint16_t rawMove);

  static uint64_t read_be_u64(const uint8_t* p);
  static uint32_t read_be_u32(const uint8_t* p);
  static uint16_t read_be_u16(const uint8_t* p);
};

#pragma once
#include <string>
#include "position.h"
#include "move.h"

// Minimal Syzygy (Fathom) wrapper.
// NOTE: requires tbprobe.c/tbchess.c to be compiled and linked.

namespace syzygy {

// Initialize tablebases from a path (can be empty). Returns true if init succeeded
// (even if no files were found; in that case TB_LARGEST will be 0).
bool init(const std::string& path);

// Free resources (optional).
void free();

// Whether tablebases are available and support at least 3+ pieces.
bool enabled();

// Largest supported piece count (0 if none).
int largest();

// Probe best move at root. Returns true if probe succeeded.
// If it succeeds, outMove is a legal move for the position.
bool probe_root(const Position& pos, Move& outMove, int& outWdl);

// Same as probe_root(), but also returns DTZ (distance-to-zeroing move)
// when DTZ tables are available.
bool probe_root_dtz(const Position& pos, Move& outMove, int& outWdl, int& outDtz);

// Probe WDL for the current position. Returns true if probe succeeded.
// outWdl in {0..4} (TB_LOSS..TB_WIN).
bool probe_wdl(const Position& pos, int& outWdl);

} // namespace syzygy

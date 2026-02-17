#pragma once
#include "position.h"

// Static Exchange Evaluation (SEE) on capture-like moves.
// Returns estimated net material gain for side to move making the move.
int see(const Position& pos, Move m);

// Convenience: return true if SEE(move) >= threshold (in centipawns).
bool see_ge(const Position& pos, Move m, int threshold);

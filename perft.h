#pragma once
#include <cstdint>
#include "position.h"

uint64_t perft(Position& pos, int depth);
uint64_t perft_root_mt(const Position& root, int depth, int threads);

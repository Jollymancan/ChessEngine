#pragma once
#include <cstdint>

extern uint64_t ZP[12][64];
extern uint64_t ZSide;
extern uint64_t ZCastle[16];
extern uint64_t ZEP[9];

void zobrist_init();

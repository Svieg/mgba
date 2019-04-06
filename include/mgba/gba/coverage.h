#ifndef COVERAGE_H
#define COVERAGE_H

#include <stdint.h>
#include <mgba/gba/map.h>

extern map_i32 covmap;
extern int covmap_started;

void cov_add_addr(int32_t addr);
#endif

#ifndef COVERAGE_H
#define COVERAGE_H

#include <stdio.h>
#include <stdint.h>
#include <mgba/gba/map.h>

extern map_i32 covmap;
extern map_i32 blmap;
extern int covmap_started;
extern int blmap_started;
extern int cpurec_started;
extern uint32_t old_gprs[16];
FILE* cpurec_file;

void cov_add_addr(int32_t addr);
void bl_add_addr(int32_t addr);
#endif

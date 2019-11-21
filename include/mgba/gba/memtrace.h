#ifndef MEMTRACE_H
#define MEMTRACE_H

#include <stdint.h>

/*
 * This api enables recording of memory accesses done by the cpu.
*/

typedef enum
{
    MEMTRACE_READ8,
    MEMTRACE_READ16,
    MEMTRACE_READ32
} MemReadType;

typedef enum
{
    MEMTRACE_WRITE8,
    MEMTRACE_WRITE16,
    MEMTRACE_WRITE32
} MemWriteType;

int memtrace_is_tracing(void);
int memtrace_record(const char *filename);
void memtrace_log_read(MemReadType rt, uint32_t pc, uint32_t address, uint32_t data);
void memtrace_log_write(MemWriteType wt, uint32_t pc, uint32_t address, uint32_t data);
void memtrace_stop(void);

#endif

#include <stdio.h>
#include <mgba/gba/memtrace.h>

static FILE* memtrace_fp = NULL;

int memtrace_is_tracing(void)
{
    return memtrace_fp != NULL;
}

int memtrace_record(const char *filename)
{
    memtrace_fp = fopen(filename, "w");

    if (!memtrace_fp)
        return -1;

    return 0;
}

void memtrace_log_read(MemReadType rt, uint32_t pc, uint32_t address, uint32_t data)
{
    if (!memtrace_is_tracing())
        return;

    // We filter out addresses outside of the ROM's code.
    if (pc < 0x8000000)
        return;

    switch (rt)
    {
        case MEMTRACE_READ8:
            fprintf(memtrace_fp, "0x%08x LOAD:8 [0x%08x] = 0x%02x\n",
                    pc - 4, address, (uint8_t)data);
            break;
        case MEMTRACE_READ16:
            fprintf(memtrace_fp, "0x%08x LOAD:16 [0x%08x] = 0x%04x\n",
                    pc - 4, address, (uint16_t)data);
            break;
        case MEMTRACE_READ32:
            fprintf(memtrace_fp, "0x%08x LOAD:32 [0x%08x] = 0x%08x\n",
                    pc - 4, address, data);
            break;
        default:
            fprintf(memtrace_fp, "Wrong read type\n");
    }
}

void memtrace_log_write(MemWriteType wt, uint32_t pc, uint32_t address, uint32_t data)
{
    if (!memtrace_is_tracing())
        return;

    // We filter out addresses outside of the ROM's code.
    if (pc < 0x8000000)
        return;

    switch (wt)
    {
        case MEMTRACE_WRITE8:
            fprintf(memtrace_fp, "0x%08x STORE:8 [0x%08x] = 0x%02x\n",
                    pc - 4, address, (uint8_t)data);
            break;
        case MEMTRACE_READ16:
            fprintf(memtrace_fp, "0x%08x STORE:16 [0x%08x] = 0x%04x\n",
                    pc - 4, address, (uint16_t)data);
            break;
        case MEMTRACE_READ32:
            fprintf(memtrace_fp, "0x%08x STORE:32 [0x%08x] = 0x%08x\n",
                    pc - 4, address, data);
            break;
        default:
            fprintf(memtrace_fp, "Wrong write type\n");
    }
}

void memtrace_stop(void)
{
    if (!memtrace_fp)
        return;

    fclose(memtrace_fp);
    memtrace_fp = NULL;
}

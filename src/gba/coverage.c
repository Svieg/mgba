#include <stdio.h>
#include <stdint.h>
#include <mgba/gba/coverage.h>
#include <mgba/gba/map.h>

map_i32 covmap;
map_i32 blmap;
int covmap_started = 0;
int blmap_started = 0;
int cpurec_started = 0;

uint32_t old_gprs[16];
FILE* cpurec_file;

static void i32_to_hex(int32_t number, char* buffer)
{
    const char* charset = "0123456789abcdef";

    for(int i = 0; i < sizeof(number); i++) {
        uint8_t idx = ((sizeof(number)*8) - 8*(i+1));
        uint8_t cur = (number >> idx) & 0xff;

        buffer[2*i] = charset[(cur >> 4) & 0x0f];
        buffer[(2*i)+1] = charset[cur & 0x0f];
    }
}

void cov_add_addr(int32_t addr)
{
    if(covmap_started > 0) {
        char key[11];
        key[10] = '\0';
        key[0] = '0';
        key[1] = 'x';
        i32_to_hex(addr, key + 2);

        int32_t* val = map_get(&covmap, key);

        if(val) {
            map_set(&covmap, key, *val + 1);
        } else {
            map_set(&covmap, key, 1);
        }
    }
}

void bl_add_addr(int32_t addr)
{
    if(blmap_started > 0) {
        char key[11];
        key[10] = '\0';
        key[0] = '0';
        key[1] = 'x';
        i32_to_hex(addr, key + 2);

        int32_t* val = map_get(&blmap, key);

        if(val) {
            map_set(&blmap, key, *val + 1);
        } else {
            map_set(&blmap, key, 1);
        }
    }
}

void cpurec_step(uint32_t instruction, uint32_t* gprs)
{
    if(cpurec_started > 0) {
        fwrite(&instruction, sizeof(uint32_t), 1, cpurec_file);

        uint16_t bitmask = 0;

        // Building bitmask
        for(int i = 0; i < 16; i++) {
            if(gprs[i] != old_gprs[i])
                bitmask |= (1 << i);
        }

        fwrite(&bitmask, sizeof(uint16_t), 1, cpurec_file);

        // Updating our gprs
        for(int i = 0; i < 16; i++) {
            if(gprs[i] != old_gprs[i])
                fwrite(&gprs[i], sizeof(uint32_t), 1, cpurec_file);

            old_gprs[i] = gprs[i];
        }
    }
}

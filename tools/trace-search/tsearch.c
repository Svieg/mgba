#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <capstone/capstone.h>

const char* usage = "./tsearch <options>\n" \
                    "options:\n" \
                    "   -i : input file (required)\n" \
                    "   -o : Output file (default stdout)\n" \
                    "   -m : Disassembly mode (arm/thumb, default none)\n" \
                    "   -f : Filter (ex: \"op=ldrh,r1=0x41414141\")\n";

const char* regs[] = {
    "r0", "r1", "r2", "r3",
    "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "fp",
    "ip", "sp", "lr", "pc"
};

typedef enum disasm_opt_t {
    DIS_MODE_NONE,
    DIS_MODE_ARM,
    DIS_MODE_THUMB
} disasm_opt;

FILE* output_file = NULL;
FILE* input_file = NULL;
csh cap_handle = 0;
long exec_step = 0;
disasm_opt disasm_mode = DIS_MODE_NONE;

static int reg_to_idx(char* input)
{
    for(int i = 0; i < 16; i++) {
        if(strcmp(input, regs[i]) == 0)
            return i;
    }

    return -1;
}

static uint32_t parse_number(char* number)
{
    int len = strlen(number);

    if(len == 0)
        return 0;

    if(len < 3)
        return strtoul(number, NULL, 10);

    if(number[0] == '0' && number[1] == 'x')
        return strtoul(number, NULL, 16);

    return strtoul(number, NULL, 10);
}

static int match_filter(char* filter, uint32_t* ins, uint32_t* gprs)
{
    // No filter == match everything
    if(filter == NULL)
        return 1;

    char* filter_ptr = filter;
    char key[64];
    char val[64];

    while(filter_ptr != NULL) {
        // Filter parsing
        char* val_start = strstr(filter_ptr, "=");
        memset(key, 0, sizeof(key));
        memset(val, 0, sizeof(val));

        // Should return an error (missing value) but we just ignore it
        if(val_start == NULL)
            return 0;

        // Get the key
        strncpy(key, filter_ptr, val_start - filter_ptr);
        val_start++;

        // Get the value
        char* next_entry = strstr(val_start, ",");

        // If we reached the last command
        if(next_entry == NULL) {
            strncpy(val, val_start, strlen(val_start));
        } else {
            strncpy(val, val_start, next_entry - val_start);
            next_entry++;
        }

        filter_ptr = next_entry;

        // Now actual filter matching
        // Opcode matching
        if(strcmp(key, "op") == 0 && cap_handle != 0) {
            cs_insn* insn;
            int count = cs_disasm(cap_handle, (const uint8_t*)ins,
                    sizeof(uint32_t), gprs[15], 0, &insn);

            if(count == 0)
                return 0;

            if(strcmp(insn[0].mnemonic, val) != 0) {
                cs_free(insn, count);
                return 0;
            }

            cs_free(insn, count);
        }

        if(strcmp(key, "idx") == 0) {
            if(exec_step == parse_number(val))
                return 1;

            return 0;
        }

        // Reg matching
        int reg_idx = reg_to_idx(key);

        if(reg_idx != -1) {
            if(gprs[reg_idx] != parse_number(val))
                return 0;
        }
    }

    return 1;
}

static void print_ins(uint32_t* instruction, uint32_t* gprs)
{
    uint32_t pc = gprs[15];
    uint8_t* insbytes = (uint8_t*)instruction;
    cs_insn* insn;

    if(disasm_mode == DIS_MODE_NONE) {
        for(int i = 0; i < sizeof(uint32_t); i++) {
            fprintf(output_file, "%02x ", insbytes[i]);
        }
    }

    if(disasm_mode == DIS_MODE_ARM || disasm_mode == DIS_MODE_THUMB) {
        int count = cs_disasm(cap_handle, (const uint8_t*)instruction,
                sizeof(uint32_t), pc, 0, &insn);

        if(count == 0) {
            fprintf(output_file, "invalid");
        } else {
            fprintf(output_file, "%s %s", insn[0].mnemonic, insn[0].op_str);
        }

        cs_free(insn, count);
    }

    fprintf(output_file, "\n");
}

static int process_file(char* filter)
{
    uint32_t instruction = 0;
    uint16_t regbitmap = 0;
    uint32_t gprs[16] = {0};

    while(fread(&instruction, sizeof(uint32_t), 1, input_file) != 0) {
        // Processing instruction
        uint8_t* insbytes = (uint8_t*)&instruction;

        // Register bitmap
        if(fread(&regbitmap, sizeof(uint16_t), 1, input_file) != 1) {
            printf("Unexpected EOF: Could not read reg bitmap\n");
            return -1;
        }

        // Updating registers
        for(int i = 0; i < 16; i++) {
            if(regbitmap & (1 << i)) {
                if(fread(&gprs[i], sizeof(uint32_t), 1, input_file) != 1) {
                    printf("Unexpected EOF: Could not read reg value\n");
                    return -1;
                }
            }
        }

        if(!match_filter(filter, &instruction, gprs)) {
            exec_step++;
            continue;
        }

        fprintf(output_file, "[Step: %lu] ", exec_step);
        print_ins(&instruction, gprs);
        // Printing registers
        for(int y = 0; y < 4; y++) {
            for(int x = 0; x < 4; x++) {
                int idx = (y*4)+x;
                fprintf(output_file, "%3s: 0x%08x ", regs[idx], gprs[idx]);
            }

            fprintf(output_file, "\n");
        }

        exec_step++;
    }

    return 0;
}

int main(int argc, char** argv)
{
    char* filter = NULL;
    output_file = stdout;

    if(argc < 3) {
        printf("%s", usage);
    }

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-i") == 0) {
            if(i + 1 == argc) {
                printf("Please specify an input file\n");
                return 1;
            }

            i++;

            FILE* fin = fopen(argv[i], "rb");

            if(fin == NULL) {
                printf("Could not open file: %s\n", argv[i]);
                return 1;
            }

            input_file = fin;
        }

        if(strcmp(argv[i], "-o") == 0) {
            if(i + 1 == argc) {
                printf("Please specify an output file\n");
                return 1;
            }

            i++;

            FILE* fout = fopen(argv[i], "wb");

            if(fout == NULL) {
                printf("Could not open file: %s\n", argv[i]);
                return 1;
            }

            output_file = fout;
        }

        if(strcmp(argv[i], "-m") == 0) {
            if(i + 1 == argc) {
                printf("Please specify an output mode\n");
                return 1;
            }

            i++;

            if(strcmp(argv[i], "arm") == 0) {
                cs_open(CS_ARCH_ARM, CS_MODE_ARM, &cap_handle);
                disasm_mode = DIS_MODE_ARM;
            } else if(strcmp(argv[i], "thumb") == 0) {
                cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &cap_handle);
                disasm_mode = DIS_MODE_THUMB;
            } else {
                printf("Please specify a valid disassembly mode (arm/thumb)\n");
                return 1;
            }
        }

        if(strcmp(argv[i], "-f") == 0) {
            if(i + 1 == argc) {
                printf("Please specify a filter\n");
                return 1;
            }

            i++;

            filter = argv[i];
        }
    }

    if(input_file == NULL) {
        printf("Please specify an input file\n");
        return 1;
    }

    process_file(filter);

    fclose(output_file);
    fclose(input_file);
    cs_close(&cap_handle);

    return 0;
}

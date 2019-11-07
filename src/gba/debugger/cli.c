/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/extra/cli.h>

#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/video.h>
#include <mgba/internal/arm/debugger/cli-debugger.h>

#include <stdio.h>
#include <mgba/gba/coverage.h>
#include <mgba/gba/map.h>
#include <mgba/gba/memtrace.h>
#include <alttp/entity.h>

static void _GBACLIDebuggerInit(struct CLIDebuggerSystem*);
static bool _GBACLIDebuggerCustom(struct CLIDebuggerSystem*);

static void _frame(struct CLIDebugger*, struct CLIDebugVector*);
static void _load(struct CLIDebugger*, struct CLIDebugVector*);
static void _save(struct CLIDebugger*, struct CLIDebugVector*);

static void _cpurecStart(struct CLIDebugger*, struct CLIDebugVector*);
static void _cpurecStop(struct CLIDebugger*, struct CLIDebugVector*);
static void _blStop(struct CLIDebugger*, struct CLIDebugVector*);
static void _blStart(struct CLIDebugger*, struct CLIDebugVector*);
static void _coverageStart(struct CLIDebugger*, struct CLIDebugVector*);
static void _coverageStop(struct CLIDebugger*, struct CLIDebugVector*);
static void _showEntities(struct CLIDebugger*, struct CLIDebugVector*);
static void _dumpWorkmem(struct CLIDebugger*, struct CLIDebugVector*);
static void _memtraceStart(struct CLIDebugger*, struct CLIDebugVector*);
static void _memtraceStop(struct CLIDebugger*, struct CLIDebugVector*);

struct CLIDebuggerCommandSummary _GBACLIDebuggerCommands[] = {
	{ "frame", _frame, "", "Frame advance" },
	{ "load", _load, "*", "Load a savestate" },
	{ "save", _save, "*", "Save a savestate" },
    { "coverage-start", _coverageStart, "", "Starts a coverage analysis"},
    { "coverage-stop", _coverageStop, "S", "Stops a coverage analysis and writes the file"},
    { "bl-start", _blStart, "", "Starts a call recording session"},
    { "bl-stop", _blStop, "S", "Stops a call recording session a writes the file"},
    { "cpurec-start", _cpurecStart, "S", "Starts recording the full execution state"},
    { "cpurec-stop", _cpurecStop, "", "Saves the recorded execution state"},
    { "memtrace-start", _memtraceStart, "S", "Starts memory access recording" },
    { "memtrace-stop", _memtraceStop, "", "Stops memory access recording" },
    { "show-entities", _showEntities, "", "Shows status info about current entities"},
    { "dump-workmem", _dumpWorkmem, "S", "Dumps the working memory of the emulator"},
	{ 0, 0, 0, 0 }
};

struct GBACLIDebugger* GBACLIDebuggerCreate(struct mCore* core) {
	struct GBACLIDebugger* debugger = malloc(sizeof(struct GBACLIDebugger));
	ARMCLIDebuggerCreate(&debugger->d);
	debugger->d.init = _GBACLIDebuggerInit;
	debugger->d.deinit = NULL;
	debugger->d.custom = _GBACLIDebuggerCustom;

	debugger->d.name = "Game Boy Advance";
	debugger->d.commands = _GBACLIDebuggerCommands;

	debugger->core = core;

	return debugger;
}

static void _GBACLIDebuggerInit(struct CLIDebuggerSystem* debugger) {
	struct GBACLIDebugger* gbaDebugger = (struct GBACLIDebugger*) debugger;

	gbaDebugger->frameAdvance = false;
}

static bool _GBACLIDebuggerCustom(struct CLIDebuggerSystem* debugger) {
	struct GBACLIDebugger* gbaDebugger = (struct GBACLIDebugger*) debugger;

	if (gbaDebugger->frameAdvance) {
		if (!gbaDebugger->inVblank && GBARegisterDISPSTATIsInVblank(((struct GBA*) gbaDebugger->core->board)->memory.io[REG_DISPSTAT >> 1])) {
			mDebuggerEnter(&gbaDebugger->d.p->d, DEBUGGER_ENTER_MANUAL, 0);
			gbaDebugger->frameAdvance = false;
			return false;
		}
		gbaDebugger->inVblank = GBARegisterDISPSTATGetInVblank(((struct GBA*) gbaDebugger->core->board)->memory.io[REG_DISPSTAT >> 1]);
		return true;
	}
	return false;
}

// Memory access recording
static void _memtraceStart(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    struct CLIDebuggerBackend* be = dbg->backend;

    if (memtrace_is_tracing())
    {
        be->printf(be, "Trace recording already started\n");
        return;
    }

    if (!dv || dv->type != CLIDV_CHAR_TYPE) {
        be->printf(be, "%s\n", ERROR_MISSING_ARGS);
        return;
    }

    if (memtrace_record(dv->charValue) == -1) {
        be->printf(be, "Could not open output file\n");
    }
}

static void _memtraceStop(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    UNUSED(dv);

    struct CLIDebuggerBackend* be = dbg->backend;

    if(!memtrace_is_tracing()) {
        be->printf(be, "Trace recording not started\n");
        return;
    }

    memtrace_stop();
}

// State recording stuff
static void _cpurecStart(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    struct CLIDebuggerBackend* be = dbg->backend;

    if(cpurec_started == 1) {
        be->printf(be, "Trace recording already running\n");
        return;
    }

    if(!dv || dv->type != CLIDV_CHAR_TYPE) {
        be->printf(be, "%s\n", ERROR_MISSING_ARGS);
        return;
    }

    if((cpurec_file = fopen(dv->charValue, "wb")) == NULL) {
        be->printf(be, "Could not open output file\n");
        return;
    }

    cpurec_started = 1;

    for(int i = 0; i < 16; i++)
        old_gprs[i] = 0;
}

static void _cpurecStop(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    UNUSED(dv);

    struct CLIDebuggerBackend* be = dbg->backend;

    if(cpurec_started == 0) {
        be->printf(be, "Trace recording not started\n");
        return;
    }

    fclose(cpurec_file);
    cpurec_file = NULL;
}

// BL recording stuff
static void _blStart(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    UNUSED(dv);
    struct CLIDebuggerBackend* be = dbg->backend;

    if(blmap_started == 1) {
        be->printf(be, "BL recording already running\n");
        return;
    }

    be->printf(be, "Starting BL recording\n");

    blmap_started = 1;
    map_init(&blmap);
}

static void _blStop(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    struct CLIDebuggerBackend* be = dbg->backend;

    if(!dv || dv->type != CLIDV_CHAR_TYPE) {
        be->printf(be, "%s\n", ERROR_MISSING_ARGS);
        return;
    }

    if(blmap_started == 0) {
        be->printf(be, "BL recording was not started\n");
        return;
    }

    char* path = dv->charValue;
    FILE* fp;

    if((fp = fopen(path, "w")) == NULL) {
        be->printf(be, "Could not open file '%s'\n", path);
        return;
    }

    if(blmap_started == 1) {
        const char* bbAddr;
        map_iter_t iter = map_iter(&blmap);

        while((bbAddr = map_next(&blmap, &iter))) {
            int* blcount = map_get(&blmap, bbAddr);
            fprintf(fp, "%s %i\n", bbAddr, *blcount);
        }

        map_deinit(&blmap);
        fclose(fp);

        blmap_started = 0;
    }

    be->printf(be, "Stopping BL recording\n");
}

// Code coverage stuff
static void _coverageStart(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    UNUSED(dv);
    struct CLIDebuggerBackend* be = dbg->backend;

    if(covmap_started == 1) {
        be->printf(be, "Coverage analysis already running\n");
        return;
    }

    be->printf(be, "Starting code coverage analysis\n");

    covmap_started = 1;
    map_init(&covmap);
}

static void _coverageStop(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    struct CLIDebuggerBackend* be = dbg->backend;

    if(!dv || dv->type != CLIDV_CHAR_TYPE) {
        be->printf(be, "%s\n", ERROR_MISSING_ARGS);
        return;
    }

    if(covmap_started == 0) {
        be->printf(be, "Coverage was not started\n");
        return;
    }

    char *path = dv->charValue;
    FILE* fp;

    if((fp = fopen(path, "w")) == NULL) {
        be->printf(be, "Could not open file '%s'\n", path);
        return;
    }

    if(covmap_started == 1) {
        const char* bbAddr;
        map_iter_t iter = map_iter(&covmap);

        while((bbAddr = map_next(&covmap, &iter))) {
            int* bbCount = map_get(&covmap, bbAddr);
            fprintf(fp, "%s %i\n", bbAddr, *bbCount);
        }

        map_deinit(&covmap);
        fclose(fp);

        covmap_started = 0;
    }

    be->printf(be, "Stopping code coverage analysis\n");
}

// Entities stuff
static void _gba_mem_read(struct CLIDebugger* dbg, uint32_t offset, uint32_t len, uint8_t* buff) {
    for(uint32_t i = 0; i < len; i++) {
        buff[i] = dbg->d.core->busRead8(dbg->d.core, offset + i);
    }
}

static void _showEntities(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    UNUSED(dv);
    const uint32_t entity_id_addr = 0x3003222;
    const uint32_t entity_hp_addr = 0x3003252;
    const uint32_t entity_low_y_pos = 0x3003102;
    const uint32_t entity_high_y_pos = 0x3003122;
    const uint32_t entity_low_x_pos = 0x3003112;
    const uint32_t entity_high_x_pos = 0x3003132;
    const uint32_t entity_map_id_addr = 0x3003095;
    const uint32_t link_x_pos_addr = 0x30038f4;
    const uint32_t link_y_pos_addr = 0x30038f0;

    struct CLIDebuggerBackend* be = dbg->backend;

    uint8_t entity_ids[16] = {0};
    uint8_t entity_hps[16] = {0};
    uint8_t entity_low_xpos[16] = {0};
    uint8_t entity_high_xpos[16] = {0};
    uint8_t entity_low_ypos[16] = {0};
    uint8_t entity_high_ypos[16] = {0};
    uint8_t entity_map_id[16] = {0};
    uint16_t link_x_pos = 0;
    uint16_t link_y_pos = 0;

    _gba_mem_read(dbg, entity_id_addr, sizeof(entity_ids), entity_ids);
    _gba_mem_read(dbg, entity_hp_addr, sizeof(entity_ids), entity_hps);
    _gba_mem_read(dbg, entity_low_x_pos, sizeof(entity_low_xpos), entity_low_xpos);
    _gba_mem_read(dbg, entity_low_y_pos, sizeof(entity_low_ypos), entity_low_ypos);
    _gba_mem_read(dbg, entity_high_x_pos, sizeof(entity_high_xpos), entity_high_xpos);
    _gba_mem_read(dbg, entity_high_y_pos, sizeof(entity_high_ypos), entity_high_ypos);
    _gba_mem_read(dbg, entity_map_id_addr, sizeof(entity_map_id), entity_map_id);
    _gba_mem_read(dbg, link_x_pos_addr, sizeof(link_x_pos), &link_x_pos);
    _gba_mem_read(dbg, link_y_pos_addr, sizeof(link_y_pos), &link_y_pos);

    be->printf(be, "--- Link ---\n");
    be->printf(be, "X = %5u Y = %5u\n", link_x_pos, link_y_pos);
    be->printf(be, "--- Game Entities ---\n");

    for(int i = 0; i < 16; i++) {
        const char* cur_name = EntityNames[entity_ids[i]];
        cur_name = (cur_name) ? cur_name : "unknown";

        uint16_t xpos = entity_high_xpos[i] << 8 | entity_low_xpos[i];
        uint16_t ypos = entity_high_ypos[i] << 8 | entity_low_ypos[i];

        be->printf(be, "MAP: %3d ID: %3u X = %5u Y = %5u HP = %3u TYPE_ID = 0x%02x NAME = %s\n",
                entity_map_id[i],
                i,
                xpos,
                ypos,
                entity_hps[i],
                entity_ids[i],
                cur_name);
    }
}

static void _dumpWorkmem(struct CLIDebugger* dbg, struct CLIDebugVector* dv) {
    struct CLIDebuggerBackend* be = dbg->backend;

    if(!dv || dv->type != CLIDV_CHAR_TYPE) {
        be->printf(be, "%s\n", ERROR_MISSING_ARGS);
        return;
    }

    char* path = dv->charValue;
    uint8_t* ramCopy = malloc(0x8000);
    
    // We copy the internal working ram (IRAM), because this is where the interesting
    // values seems to be.
    _gba_mem_read(dbg, 0x3000000, 0x8000, ramCopy);

    FILE* fp = fopen(path, "wb");

    if(fp == NULL) {
        be->printf(be, "Could not open file '%s'\n", path);
        return;
    }

    fwrite(ramCopy, 0x8000, 1, fp);
    free(ramCopy);
    fclose(fp);
}

static void _frame(struct CLIDebugger* debugger, struct CLIDebugVector* dv) {
	UNUSED(dv);
	debugger->d.state = DEBUGGER_CUSTOM;

	struct GBACLIDebugger* gbaDebugger = (struct GBACLIDebugger*) debugger->system;
	gbaDebugger->frameAdvance = true;
	gbaDebugger->inVblank = GBARegisterDISPSTATGetInVblank(((struct GBA*) gbaDebugger->core->board)->memory.io[REG_DISPSTAT >> 1]);
}

static void _load(struct CLIDebugger* debugger, struct CLIDebugVector* dv) {
	struct CLIDebuggerBackend* be = debugger->backend;
	if (!dv || dv->type != CLIDV_INT_TYPE) {
		be->printf(be, "%s\n", ERROR_MISSING_ARGS);
		return;
	}

	int state = dv->intValue;
	if (state < 1 || state > 9) {
		be->printf(be, "State %u out of range", state);
	}

	struct GBACLIDebugger* gbaDebugger = (struct GBACLIDebugger*) debugger->system;

	mCoreLoadState(gbaDebugger->core, dv->intValue, SAVESTATE_SCREENSHOT | SAVESTATE_RTC);
}

// TODO: Put back rewind

static void _save(struct CLIDebugger* debugger, struct CLIDebugVector* dv) {
	struct CLIDebuggerBackend* be = debugger->backend;
	if (!dv || dv->type != CLIDV_INT_TYPE) {
		be->printf(be, "%s\n", ERROR_MISSING_ARGS);
		return;
	}

	int state = dv->intValue;
	if (state < 1 || state > 9) {
		be->printf(be, "State %u out of range", state);
	}

	struct GBACLIDebugger* gbaDebugger = (struct GBACLIDebugger*) debugger->system;

	mCoreSaveState(gbaDebugger->core, dv->intValue, SAVESTATE_SCREENSHOT | SAVESTATE_RTC | SAVESTATE_METADATA);
}

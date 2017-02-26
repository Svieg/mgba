/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/ds/renderers/software.h>
#include "gba/renderers/software-private.h"

#include <mgba/internal/arm/macros.h>
#include <mgba/internal/ds/io.h>

static void DSVideoSoftwareRendererInit(struct DSVideoRenderer* renderer);
static void DSVideoSoftwareRendererDeinit(struct DSVideoRenderer* renderer);
static void DSVideoSoftwareRendererReset(struct DSVideoRenderer* renderer);
static uint16_t DSVideoSoftwareRendererWriteVideoRegister(struct DSVideoRenderer* renderer, uint32_t address, uint16_t value);
static void DSVideoSoftwareRendererWritePalette(struct DSVideoRenderer* renderer, uint32_t address, uint16_t value);
static void DSVideoSoftwareRendererWriteOAM(struct DSVideoRenderer* renderer, uint32_t oam);
static void DSVideoSoftwareRendererInvalidateExtPal(struct DSVideoRenderer* renderer, bool obj, bool engB, int slot);
static void DSVideoSoftwareRendererDrawScanline(struct DSVideoRenderer* renderer, int y);
static void DSVideoSoftwareRendererFinishFrame(struct DSVideoRenderer* renderer);
static void DSVideoSoftwareRendererGetPixels(struct DSVideoRenderer* renderer, size_t* stride, const void** pixels);
static void DSVideoSoftwareRendererPutPixels(struct DSVideoRenderer* renderer, size_t stride, const void* pixels);

static bool _regenerateExtPalette(struct DSVideoSoftwareRenderer* renderer, bool engB, int slot) {
	color_t* palette;
	color_t* variantPalette;
	struct GBAVideoSoftwareRenderer* softwareRenderer;
	uint16_t* vram;
	if (!engB) {
		palette = &renderer->extPaletteA[slot * 4096];
		variantPalette = &renderer->variantPaletteA[slot * 4096];
		softwareRenderer = &renderer->engA;
		vram = renderer->d.vramABGExtPal[slot];
	} else {
		palette = &renderer->extPaletteB[slot * 4096];
		variantPalette = &renderer->variantPaletteB[slot * 4096];
		softwareRenderer = &renderer->engB;
		vram = renderer->d.vramBBGExtPal[slot];
	}
	if (!vram) {
		return false;
	}
	int i;
	for (i = 0; i < 4096; ++i) {
		uint16_t value = vram[i];
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
		unsigned color = 0;
		color |= (value & 0x001F) << 11;
		color |= (value & 0x03E0) << 1;
		color |= (value & 0x7C00) >> 10;
#else
		unsigned color = value;
#endif
#else
		unsigned color = 0;
		color |= (value << 3) & 0xF8;
		color |= (value << 6) & 0xF800;
		color |= (value << 9) & 0xF80000;
		color |= (color >> 5) & 0x070707;
#endif
		palette[i] = color;
		if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
			variantPalette[i] = _brighten(color, softwareRenderer->bldy);
		} else if (softwareRenderer->blendEffect == BLEND_DARKEN) {
			variantPalette[i] = _darken(color, softwareRenderer->bldy);
		}
	}
	return true;
}


void DSVideoSoftwareRendererCreate(struct DSVideoSoftwareRenderer* renderer) {
	renderer->d.init = DSVideoSoftwareRendererInit;
	renderer->d.reset = DSVideoSoftwareRendererReset;
	renderer->d.deinit = DSVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = DSVideoSoftwareRendererWriteVideoRegister;
	renderer->d.writePalette = DSVideoSoftwareRendererWritePalette;
	renderer->d.writeOAM = DSVideoSoftwareRendererWriteOAM;
	renderer->d.invalidateExtPal = DSVideoSoftwareRendererInvalidateExtPal;
	renderer->d.drawScanline = DSVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = DSVideoSoftwareRendererFinishFrame;
	renderer->d.getPixels = DSVideoSoftwareRendererGetPixels;
	renderer->d.putPixels = DSVideoSoftwareRendererPutPixels;

	renderer->engA.d.cache = NULL;
	GBAVideoSoftwareRendererCreate(&renderer->engA);
	renderer->engB.d.cache = NULL;
	GBAVideoSoftwareRendererCreate(&renderer->engB);
}

static void DSVideoSoftwareRendererInit(struct DSVideoRenderer* renderer) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	softwareRenderer->engA.d.palette = &renderer->palette[0];
	softwareRenderer->engA.d.oam = &renderer->oam->oam[0];
	softwareRenderer->engA.masterEnd = DS_VIDEO_HORIZONTAL_PIXELS;
	softwareRenderer->engA.masterHeight = DS_VIDEO_VERTICAL_PIXELS;
	softwareRenderer->engA.masterScanlines = DS_VIDEO_VERTICAL_TOTAL_PIXELS;
	softwareRenderer->engA.outputBufferStride = softwareRenderer->outputBufferStride;
	softwareRenderer->engB.d.palette = &renderer->palette[512];
	softwareRenderer->engB.d.oam = &renderer->oam->oam[1];
	softwareRenderer->engB.masterEnd = DS_VIDEO_HORIZONTAL_PIXELS;
	softwareRenderer->engB.masterHeight = DS_VIDEO_VERTICAL_PIXELS;
	softwareRenderer->engB.masterScanlines = DS_VIDEO_VERTICAL_TOTAL_PIXELS;
	softwareRenderer->engB.outputBufferStride = softwareRenderer->outputBufferStride;

	DSVideoSoftwareRendererReset(renderer);
}

static void DSVideoSoftwareRendererReset(struct DSVideoRenderer* renderer) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	softwareRenderer->engA.d.reset(&softwareRenderer->engA.d);
	softwareRenderer->engB.d.reset(&softwareRenderer->engB.d);
	softwareRenderer->powcnt = 0;
	softwareRenderer->dispcntA = 0;
	softwareRenderer->dispcntB = 0;
}

static void DSVideoSoftwareRendererDeinit(struct DSVideoRenderer* renderer) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	softwareRenderer->engA.d.deinit(&softwareRenderer->engA.d);
	softwareRenderer->engB.d.deinit(&softwareRenderer->engB.d);
}

static void DSVideoSoftwareRendererUpdateDISPCNT(struct DSVideoSoftwareRenderer* softwareRenderer, bool engB) {
	uint32_t dispcnt;
	struct GBAVideoSoftwareRenderer* eng;
	if (!engB) {
		dispcnt = softwareRenderer->dispcntA;
		eng = &softwareRenderer->engA;
	} else {
		dispcnt = softwareRenderer->dispcntB;
		eng = &softwareRenderer->engB;
	}
	uint16_t fakeDispcnt = dispcnt & 0xFF87;
	if (!DSRegisterDISPCNTIsTileObjMapping(dispcnt)) {
		eng->tileStride = 0x20;
	} else {
		eng->tileStride = 0x20 << DSRegisterDISPCNTGetTileBoundary(dispcnt);
		fakeDispcnt = GBARegisterDISPCNTFillObjCharacterMapping(fakeDispcnt);
	}
	eng->d.writeVideoRegister(&eng->d, DS9_REG_A_DISPCNT_LO, fakeDispcnt);
	eng->dispcnt |= dispcnt & 0xFFFF0000;
	if (DSRegisterDISPCNTIsBgExtPalette(dispcnt)) {
		color_t* extPalette;
		if (!engB) {
			extPalette = softwareRenderer->extPaletteA;
		} else {
			extPalette = softwareRenderer->extPaletteB;
		}
		int i;
		for (i = 0; i < 4; ++i) {
			int slot = i;
			if (i < 2 && GBARegisterBGCNTIsExtPaletteSlot(eng->bg[i].control)) {
				slot += 2;
			}
			if (eng->bg[i].extPalette != &extPalette[slot * 4096] && _regenerateExtPalette(softwareRenderer, engB, slot)) {
				eng->bg[i].extPalette = &extPalette[slot * 4096];
			}
		}
	} else {
		eng->bg[0].extPalette = NULL;
		eng->bg[1].extPalette = NULL;
		eng->bg[2].extPalette = NULL;
		eng->bg[3].extPalette = NULL;
	}
	if (!engB) {
		uint32_t charBase = DSRegisterDISPCNTGetCharBase(softwareRenderer->dispcntA) << 16;
		uint32_t screenBase = DSRegisterDISPCNTGetScreenBase(softwareRenderer->dispcntA) << 16;
		softwareRenderer->engA.d.writeVideoRegister(&softwareRenderer->engA.d, DS9_REG_A_BG0CNT, softwareRenderer->engA.bg[0].control);
		softwareRenderer->engA.bg[0].charBase += charBase;
		softwareRenderer->engA.bg[0].screenBase &= ~0x70000;
		softwareRenderer->engA.bg[0].screenBase |= screenBase;
		softwareRenderer->engA.d.writeVideoRegister(&softwareRenderer->engA.d, DS9_REG_A_BG1CNT, softwareRenderer->engA.bg[1].control);
		softwareRenderer->engA.bg[1].charBase += charBase;
		softwareRenderer->engA.bg[1].screenBase &= ~0x70000;
		softwareRenderer->engA.bg[1].screenBase |= screenBase;
		softwareRenderer->engA.d.writeVideoRegister(&softwareRenderer->engA.d, DS9_REG_A_BG2CNT, softwareRenderer->engA.bg[2].control);
		softwareRenderer->engA.bg[2].charBase += charBase;
		softwareRenderer->engA.bg[2].screenBase &= ~0x70000;
		softwareRenderer->engA.bg[2].screenBase |= screenBase;
		softwareRenderer->engA.d.writeVideoRegister(&softwareRenderer->engA.d, DS9_REG_A_BG3CNT, softwareRenderer->engA.bg[3].control);
		softwareRenderer->engA.bg[3].charBase += charBase;
		softwareRenderer->engA.bg[3].screenBase &= ~0x70000;
		softwareRenderer->engA.bg[3].screenBase |= screenBase;
	}
}

static uint16_t DSVideoSoftwareRendererWriteVideoRegister(struct DSVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	if (address >= DS9_REG_A_BG0CNT && address <= DS9_REG_A_BLDY) {
		softwareRenderer->engA.d.writeVideoRegister(&softwareRenderer->engA.d, address, value);
	} else if (address >= DS9_REG_B_BG0CNT && address <= DS9_REG_B_BLDY) {
		softwareRenderer->engB.d.writeVideoRegister(&softwareRenderer->engB.d, address & 0xFF, value);
	} else {
		mLOG(DS_VIDEO, STUB, "Stub video register write: %04X:%04X", address, value);
	}
	switch (address) {
	case DS9_REG_A_BG0CNT:
	case DS9_REG_A_BG1CNT:
		softwareRenderer->engA.bg[(address - DS9_REG_A_BG0CNT) >> 1].control = value;
		break;
	case DS9_REG_B_BG0CNT:
	case DS9_REG_B_BG1CNT:
		softwareRenderer->engB.bg[(address - DS9_REG_A_BG0CNT) >> 1].control = value;
		break;
	case DS9_REG_A_DISPCNT_LO:
		softwareRenderer->dispcntA &= 0xFFFF0000;
		softwareRenderer->dispcntA |= value;
		DSVideoSoftwareRendererUpdateDISPCNT(softwareRenderer, false);
		break;
	case DS9_REG_A_DISPCNT_HI:
		softwareRenderer->dispcntA &= 0x0000FFFF;
		softwareRenderer->dispcntA |= value << 16;
		DSVideoSoftwareRendererUpdateDISPCNT(softwareRenderer, false);
		break;
	case DS9_REG_B_DISPCNT_LO:
		softwareRenderer->dispcntB &= 0xFFFF0000;
		softwareRenderer->dispcntB |= value;
		DSVideoSoftwareRendererUpdateDISPCNT(softwareRenderer, true);
		break;
	case DS9_REG_B_DISPCNT_HI:
		softwareRenderer->dispcntB &= 0x0000FFFF;
		softwareRenderer->dispcntB |= value << 16;
		DSVideoSoftwareRendererUpdateDISPCNT(softwareRenderer, true);
		break;
	case DS9_REG_POWCNT1:
		value &= 0x810F;
		softwareRenderer->powcnt = value;
	}
	return value;
}

static void DSVideoSoftwareRendererWritePalette(struct DSVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	if (address < 0x400) {
		softwareRenderer->engA.d.writePalette(&softwareRenderer->engA.d, address & 0x3FF, value);
	} else {
		softwareRenderer->engB.d.writePalette(&softwareRenderer->engB.d, address & 0x3FF, value);
	}
}

static void DSVideoSoftwareRendererWriteOAM(struct DSVideoRenderer* renderer, uint32_t oam) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	if (oam < 0x200) {
		softwareRenderer->engA.d.writeOAM(&softwareRenderer->engA.d, oam & 0x1FF);
	} else {
		softwareRenderer->engB.d.writeOAM(&softwareRenderer->engB.d, oam & 0x1FF);
	}
}

static void DSVideoSoftwareRendererInvalidateExtPal(struct DSVideoRenderer* renderer, bool obj, bool engB, int slot) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	_regenerateExtPalette(softwareRenderer, engB, slot);
}

static void DSVideoSoftwareRendererDrawGBAScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	color_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (GBARegisterDISPCNTIsForcedBlank(softwareRenderer->dispcnt)) {
		int x;
		for (x = 0; x < softwareRenderer->masterEnd; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	}

	GBAVideoSoftwareRendererPreprocessBuffer(softwareRenderer, y);
	int spriteLayers = GBAVideoSoftwareRendererPreprocessSpriteLayer(softwareRenderer, y);

	int w;
	unsigned priority;
	for (priority = 0; priority < 4; ++priority) {
		softwareRenderer->end = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			softwareRenderer->start = softwareRenderer->end;
			softwareRenderer->end = softwareRenderer->windows[w].endX;
			softwareRenderer->currentWindow = softwareRenderer->windows[w].control;
			if (spriteLayers & (1 << priority)) {
				GBAVideoSoftwareRendererPostprocessSprite(softwareRenderer, priority);
			}
			if (TEST_LAYER_ENABLED(0)) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[0], y);
			}
			if (TEST_LAYER_ENABLED(1)) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[1], y);
			}
			if (TEST_LAYER_ENABLED(2)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
				case 1:
				case 3:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 2:
				case 4:
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				}
			}
			if (TEST_LAYER_ENABLED(3)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[3], y);
					break;
				case 1:
				case 2:
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[3], y);
					break;
				}
			}
		}
	}
	softwareRenderer->bg[2].sx += softwareRenderer->bg[2].dmx;
	softwareRenderer->bg[2].sy += softwareRenderer->bg[2].dmy;
	softwareRenderer->bg[3].sx += softwareRenderer->bg[3].dmx;
	softwareRenderer->bg[3].sy += softwareRenderer->bg[3].dmy;

	GBAVideoSoftwareRendererPostprocessBuffer(softwareRenderer);

#ifdef COLOR_16_BIT
#if defined(__ARM_NEON) && !defined(__APPLE__)
	_to16Bit(row, softwareRenderer->row, softwareRenderer->masterEnd);
#else
	for (x = 0; x < softwareRenderer->masterEnd; ++x) {
		row[x] = softwareRenderer->row[x];
	}
#endif
#else
	memcpy(row, softwareRenderer->row, softwareRenderer->masterEnd * sizeof(*row));
#endif
}

static void _drawScanlineA(struct DSVideoSoftwareRenderer* softwareRenderer, int y) {
	memcpy(softwareRenderer->engA.d.vramBG, softwareRenderer->d.vramABG, sizeof(softwareRenderer->engA.d.vramBG));
	memcpy(softwareRenderer->engA.d.vramOBJ, softwareRenderer->d.vramAOBJ, sizeof(softwareRenderer->engA.d.vramOBJ));
	color_t* row = &softwareRenderer->engA.outputBuffer[softwareRenderer->outputBufferStride * y];

	int x;
	switch (DSRegisterDISPCNTGetDispMode(softwareRenderer->dispcntA)) {
	case 0:
		for (x = 0; x < DS_VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	case 1:
		DSVideoSoftwareRendererDrawGBAScanline(&softwareRenderer->engA.d, y);
		return;
	case 2: {
		uint16_t* vram = &softwareRenderer->d.vram[0x10000 * DSRegisterDISPCNTGetVRAMBlock(softwareRenderer->dispcntA)];
		for (x = 0; x < DS_VIDEO_HORIZONTAL_PIXELS; ++x) {
			color_t color;
			LOAD_16(color, (x + y * DS_VIDEO_HORIZONTAL_PIXELS) * 2, vram);
#ifndef COLOR_16_BIT
			unsigned color32 = 0;
			color32 |= (color << 9) & 0xF80000;
			color32 |= (color << 3) & 0xF8;
			color32 |= (color << 6) & 0xF800;
			color32 |= (color32 >> 5) & 0x070707;
			color = color32;
#elif COLOR_5_6_5
			uint16_t color16 = 0;
			color16 |= (color & 0x001F) << 11;
			color16 |= (color & 0x03E0) << 1;
			color16 |= (color & 0x7C00) >> 10;
			color = color16;
#endif
			softwareRenderer->row[x] = color;
		}
		break;
	}
	case 3:
		break;
	}

#ifdef COLOR_16_BIT
#if defined(__ARM_NEON) && !defined(__APPLE__)
	_to16Bit(row, softwareRenderer->row, DS_VIDEO_HORIZONTAL_PIXELS);
#else
	for (x = 0; x < DS_VIDEO_HORIZONTAL_PIXELS; ++x) {
		row[x] = softwareRenderer->row[x];
	}
#endif
#else
	memcpy(row, softwareRenderer->row, DS_VIDEO_HORIZONTAL_PIXELS * sizeof(*row));
#endif
}

static void _drawScanlineB(struct DSVideoSoftwareRenderer* softwareRenderer, int y) {
	memcpy(softwareRenderer->engB.d.vramBG, softwareRenderer->d.vramBBG, sizeof(softwareRenderer->engB.d.vramBG));
	memcpy(softwareRenderer->engB.d.vramOBJ, softwareRenderer->d.vramBOBJ, sizeof(softwareRenderer->engB.d.vramOBJ));
	color_t* row = &softwareRenderer->engB.outputBuffer[softwareRenderer->outputBufferStride * y];

	int x;
	switch (DSRegisterDISPCNTGetDispMode(softwareRenderer->dispcntB)) {
	case 0:
		for (x = 0; x < DS_VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	case 1:
		DSVideoSoftwareRendererDrawGBAScanline(&softwareRenderer->engB.d, y);
		return;
	}

#ifdef COLOR_16_BIT
#if defined(__ARM_NEON) && !defined(__APPLE__)
	_to16Bit(row, softwareRenderer->row, DS_VIDEO_HORIZONTAL_PIXELS);
#else
	for (x = 0; x < DS_VIDEO_HORIZONTAL_PIXELS; ++x) {
		row[x] = softwareRenderer->row[x];
	}
#endif
#else
	memcpy(row, softwareRenderer->row, DS_VIDEO_HORIZONTAL_PIXELS * sizeof(*row));
#endif
}

static void DSVideoSoftwareRendererDrawScanline(struct DSVideoRenderer* renderer, int y) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	if (!DSRegisterPOWCNT1IsSwap(softwareRenderer->powcnt)) {
		softwareRenderer->engA.outputBuffer = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * DS_VIDEO_VERTICAL_PIXELS];
		softwareRenderer->engB.outputBuffer = softwareRenderer->outputBuffer;
	} else {
		softwareRenderer->engA.outputBuffer = softwareRenderer->outputBuffer;
		softwareRenderer->engB.outputBuffer = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * DS_VIDEO_VERTICAL_PIXELS];
	}

	_drawScanlineA(softwareRenderer, y);
	_drawScanlineB(softwareRenderer, y);
}

static void DSVideoSoftwareRendererFinishFrame(struct DSVideoRenderer* renderer) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
	softwareRenderer->engA.d.finishFrame(&softwareRenderer->engA.d);
	softwareRenderer->engB.d.finishFrame(&softwareRenderer->engB.d);
}

static void DSVideoSoftwareRendererGetPixels(struct DSVideoRenderer* renderer, size_t* stride, const void** pixels) {
	struct DSVideoSoftwareRenderer* softwareRenderer = (struct DSVideoSoftwareRenderer*) renderer;
#ifdef COLOR_16_BIT
#error Not yet supported
#else
	*stride = softwareRenderer->outputBufferStride;
	*pixels = softwareRenderer->outputBuffer;
#endif
}

static void DSVideoSoftwareRendererPutPixels(struct DSVideoRenderer* renderer, size_t stride, const void* pixels) {
}

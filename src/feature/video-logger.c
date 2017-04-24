/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "video-logger.h"

#include <mgba/core/core.h>
#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>
#include <mgba-util/math.h>

#ifdef M_CORE_GBA
#include <mgba/gba/core.h>
#endif
#ifdef M_CORE_GB
#include <mgba/gb/core.h>
#endif

#ifdef USE_ZLIB
#include <zlib.h>
#endif

#define BUFFER_BASE_SIZE 0x20000

const char mVL_MAGIC[] = "mVL\0";

const static struct mVLDescriptor {
	enum mPlatform platform;
	struct mCore* (*open)(void);
} _descriptors[] = {
#ifdef M_CORE_GBA
	{ PLATFORM_GBA, GBAVideoLogPlayerCreate },
#endif
#ifdef M_CORE_GB
	{ PLATFORM_GB, GBVideoLogPlayerCreate },
#endif
	{ PLATFORM_NONE, 0 }
};

enum mVLBlockType {
	mVL_BLOCK_DUMMY = 0,
	mVL_BLOCK_INITIAL_STATE,
	mVL_BLOCK_CHANNEL_HEADER,
	mVL_BLOCK_DATA,
	mVL_BLOCK_FOOTER = 0x784C566D
};

enum mVLHeaderFlag {
	mVL_FLAG_HAS_INITIAL_STATE = 1
};

struct mVLBlockHeader {
	uint32_t blockType;
	uint32_t length;
	uint32_t channelId;
	uint32_t flags;
};

enum mVLBlockFlag {
	mVL_FLAG_BLOCK_COMPRESSED = 1
};

struct mVideoLogHeader {
	char magic[4];
	uint32_t flags;
	uint32_t platform;
	uint32_t nChannels;
};

struct mVideoLogContext;
struct mVideoLogChannel {
	struct mVideoLogContext* p;

	uint32_t type;
	void* initialState;
	size_t initialStateSize;

	off_t currentPointer;
	size_t bufferRemaining;
#ifdef USE_ZLIB
	bool inflating;
	z_stream inflateStream;
#endif

	struct CircleBuffer buffer;
};

struct mVideoLogContext {
	void* initialState;
	size_t initialStateSize;
	uint32_t nChannels;
	struct mVideoLogChannel channels[mVL_MAX_CHANNELS];

	bool write;
	uint32_t activeChannel;
	struct VFile* backing;
};


static bool _writeData(struct mVideoLogger* logger, const void* data, size_t length);
static bool _writeNull(struct mVideoLogger* logger, const void* data, size_t length);
static bool _readData(struct mVideoLogger* logger, void* data, size_t length, bool block);

static ssize_t mVideoLoggerReadChannel(struct mVideoLogChannel* channel, void* data, size_t length);
static ssize_t mVideoLoggerWriteChannel(struct mVideoLogChannel* channel, const void* data, size_t length);

static inline size_t _roundUp(size_t value, int shift) {
	value += (1 << shift) - 1;
	return value >> shift;
}

void mVideoLoggerRendererCreate(struct mVideoLogger* logger, bool readonly) {
	if (readonly) {
		logger->writeData = _writeNull;
		logger->block = true;
	} else {
		logger->writeData = _writeData;
	}
	logger->readData = _readData;
	logger->dataContext = NULL;

	logger->init = NULL;
	logger->deinit = NULL;
	logger->reset = NULL;

	logger->lock = NULL;
	logger->unlock = NULL;
	logger->wait = NULL;
	logger->wake = NULL;
}

void mVideoLoggerRendererInit(struct mVideoLogger* logger) {
	logger->palette = anonymousMemoryMap(logger->paletteSize);
	logger->vram = anonymousMemoryMap(logger->vramSize);
	logger->oam = anonymousMemoryMap(logger->oamSize);

	logger->vramDirtyBitmap = calloc(_roundUp(logger->vramSize, 17), sizeof(uint32_t));
	logger->oamDirtyBitmap = calloc(_roundUp(logger->oamSize, 6), sizeof(uint32_t));

	if (logger->init) {
		logger->init(logger);
	}
}

void mVideoLoggerRendererDeinit(struct mVideoLogger* logger) {
	if (logger->deinit) {
		logger->deinit(logger);
	}

	mappedMemoryFree(logger->palette, logger->paletteSize);
	mappedMemoryFree(logger->vram, logger->vramSize);
	mappedMemoryFree(logger->oam, logger->oamSize);

	free(logger->vramDirtyBitmap);
	free(logger->oamDirtyBitmap);
}

void mVideoLoggerRendererReset(struct mVideoLogger* logger) {
	memset(logger->vramDirtyBitmap, 0, sizeof(uint32_t) * _roundUp(logger->vramSize, 17));
	memset(logger->oamDirtyBitmap, 0, sizeof(uint32_t) * _roundUp(logger->oamSize, 6));

	if (logger->reset) {
		logger->reset(logger);
	}
}

void mVideoLoggerRendererWriteVideoRegister(struct mVideoLogger* logger, uint32_t address, uint16_t value) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_REGISTER,
		address,
		value,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerRendererWriteVRAM(struct mVideoLogger* logger, uint32_t address) {
	int bit = 1 << (address >> 12);
	if (logger->vramDirtyBitmap[address >> 17] & bit) {
		return;
	}
	logger->vramDirtyBitmap[address >> 17] |= bit;
}

void mVideoLoggerRendererWritePalette(struct mVideoLogger* logger, uint32_t address, uint16_t value) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_PALETTE,
		address,
		value,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerRendererWriteOAM(struct mVideoLogger* logger, uint32_t address, uint16_t value) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_OAM,
		address,
		value,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

static void _flushVRAM(struct mVideoLogger* logger) {
	size_t i;
	for (i = 0; i < _roundUp(logger->vramSize, 17); ++i) {
		if (logger->vramDirtyBitmap[i]) {
			uint32_t bitmap = logger->vramDirtyBitmap[i];
			logger->vramDirtyBitmap[i] = 0;
			int j;
			for (j = 0; j < mVL_MAX_CHANNELS; ++j) {
				if (!(bitmap & (1 << j))) {
					continue;
				}
				struct mVideoLoggerDirtyInfo dirty = {
					DIRTY_VRAM,
					j * 0x1000,
					0x1000,
					0xDEADBEEF,
				};
				logger->writeData(logger, &dirty, sizeof(dirty));
				logger->writeData(logger, logger->vramBlock(logger, j * 0x1000), 0x1000);
			}
		}
	}
}

void mVideoLoggerRendererDrawScanline(struct mVideoLogger* logger, int y) {
	_flushVRAM(logger);
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_SCANLINE,
		y,
		0,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerRendererDrawRange(struct mVideoLogger* logger, int startX, int endX, int y) {
	_flushVRAM(logger);
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_RANGE,
		y,
		startX,
		endX,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerRendererFlush(struct mVideoLogger* logger) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_FLUSH,
		0,
		0,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerRendererFinishFrame(struct mVideoLogger* logger) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_FRAME,
		0,
		0,
		0xDEADBEEF,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
}

void mVideoLoggerWriteBuffer(struct mVideoLogger* logger, uint32_t bufferId, uint32_t offset, uint32_t length, const void* data) {
	struct mVideoLoggerDirtyInfo dirty = {
		DIRTY_BUFFER,
		bufferId,
		offset,
		length,
	};
	logger->writeData(logger, &dirty, sizeof(dirty));
	logger->writeData(logger, data, length);
}

bool mVideoLoggerRendererRun(struct mVideoLogger* logger, bool block) {
	struct mVideoLoggerDirtyInfo item = {0};
	while (logger->readData(logger, &item, sizeof(item), block)) {
		switch (item.type) {
		case DIRTY_REGISTER:
		case DIRTY_PALETTE:
		case DIRTY_OAM:
		case DIRTY_VRAM:
		case DIRTY_SCANLINE:
		case DIRTY_FLUSH:
		case DIRTY_FRAME:
		case DIRTY_RANGE:
		case DIRTY_BUFFER:
			if (!logger->parsePacket(logger, &item)) {
				return true;
			}
			break;
		default:
			return false;
		}
	}
	return !block;
}

static bool _writeData(struct mVideoLogger* logger, const void* data, size_t length) {
	struct mVideoLogChannel* channel = logger->dataContext;
	return mVideoLoggerWriteChannel(channel, data, length) == (ssize_t) length;
}

static bool _writeNull(struct mVideoLogger* logger, const void* data, size_t length) {
	UNUSED(logger);
	UNUSED(data);
	UNUSED(length);
	return false;
}

static bool _readData(struct mVideoLogger* logger, void* data, size_t length, bool block) {
	UNUSED(block);
	struct mVideoLogChannel* channel = logger->dataContext;
	return mVideoLoggerReadChannel(channel, data, length) == (ssize_t) length;
}

void mVideoLoggerAttachChannel(struct mVideoLogger* logger, struct mVideoLogContext* context, size_t channelId) {
	if (channelId >= mVL_MAX_CHANNELS) {
		return;
	}
	logger->dataContext = &context->channels[channelId];
}

struct mVideoLogContext* mVideoLogContextCreate(struct mCore* core) {
	struct mVideoLogContext* context = malloc(sizeof(*context));
	memset(context, 0, sizeof(*context));

	context->write = !!core;

	if (core) {
		context->initialStateSize = core->stateSize(core);
		context->initialState = anonymousMemoryMap(context->initialStateSize);
		core->saveState(core, context->initialState);
		core->startVideoLog(core, context);
	}

	context->activeChannel = 0;
	return context;
}

void mVideoLogContextSetOutput(struct mVideoLogContext* context, struct VFile* vf) {
	context->backing = vf;
	vf->truncate(vf, 0);
	vf->seek(vf, 0, SEEK_SET);
}

void mVideoLogContextWriteHeader(struct mVideoLogContext* context, struct mCore* core) {
	struct mVideoLogHeader header = { { 0 } };
	memcpy(header.magic, mVL_MAGIC, sizeof(header.magic));
	enum mPlatform platform = core->platform(core);
	STORE_32LE(platform, 0, &header.platform);
	STORE_32LE(context->nChannels, 0, &header.nChannels);

	uint32_t flags = 0;
	if (context->initialState) {
		flags |= mVL_FLAG_HAS_INITIAL_STATE;
	}
	STORE_32LE(flags, 0, &header.flags);
	context->backing->write(context->backing, &header, sizeof(header));
	if (context->initialState) {
		struct mVLBlockHeader chheader = { 0 };
		STORE_32LE(mVL_BLOCK_INITIAL_STATE, 0, &chheader.blockType);
		STORE_32LE(context->initialStateSize, 0, &chheader.length);
		context->backing->write(context->backing, &chheader, sizeof(chheader));
		context->backing->write(context->backing, context->initialState, context->initialStateSize);
	}

 	size_t i;
	for (i = 0; i < context->nChannels; ++i) {
		struct mVLBlockHeader chheader = { 0 };
		STORE_32LE(mVL_BLOCK_CHANNEL_HEADER, 0, &chheader.blockType);
		STORE_32LE(i, 0, &chheader.channelId);
		context->backing->write(context->backing, &chheader, sizeof(chheader));
	}
}

bool _readBlockHeader(struct mVideoLogContext* context, struct mVLBlockHeader* header) {
	struct mVLBlockHeader buffer;
	if (context->backing->read(context->backing, &buffer, sizeof(buffer)) != sizeof(buffer)) {
		return false;
	}
	LOAD_32LE(header->blockType, 0, &buffer.blockType);
	LOAD_32LE(header->length, 0, &buffer.length);
	LOAD_32LE(header->channelId, 0, &buffer.channelId);
	LOAD_32LE(header->flags, 0, &buffer.flags);
	return true;
}

bool _readHeader(struct mVideoLogContext* context) {
	struct mVideoLogHeader header;
	context->backing->seek(context->backing, 0, SEEK_SET);
	if (context->backing->read(context->backing, &header, sizeof(header)) != sizeof(header)) {
		return false;
	}
	if (memcmp(header.magic, mVL_MAGIC, sizeof(header.magic)) != 0) {
		return false;
	}

	LOAD_32LE(context->nChannels, 0, &header.nChannels);
	if (context->nChannels > mVL_MAX_CHANNELS) {
		return false;
	}

	uint32_t flags;
	LOAD_32LE(flags, 0, &header.flags);
	if (flags & mVL_FLAG_HAS_INITIAL_STATE) {
		struct mVLBlockHeader header;
		if (!_readBlockHeader(context, &header)) {
			return false;
		}
		if (header.blockType != mVL_BLOCK_INITIAL_STATE) {
			return false;
		}
		context->initialStateSize = header.length;
		context->initialState = anonymousMemoryMap(header.length);
		context->backing->read(context->backing, context->initialState, context->initialStateSize);
	}
	return true;
}

bool mVideoLogContextLoad(struct mVideoLogContext* context, struct VFile* vf) {
	context->backing = vf;

	if (!_readHeader(context)) {
		return false;
	}

	off_t pointer = context->backing->seek(context->backing, 0, SEEK_CUR);

	size_t i;
	for (i = 0; i < context->nChannels; ++i) {
		CircleBufferInit(&context->channels[i].buffer, BUFFER_BASE_SIZE);
		context->channels[i].bufferRemaining = 0;
		context->channels[i].currentPointer = pointer;
		context->channels[i].p = context;
#ifdef USE_ZLIB
		context->channels[i].inflating = false;
#endif
	}
	return true;
}

#ifdef USE_ZLIB
static void _flushBufferCompressed(struct mVideoLogContext* context) {
	struct CircleBuffer* buffer = &context->channels[context->activeChannel].buffer;
	if (!CircleBufferSize(buffer)) {
		return;
	}
	uint8_t writeBuffer[0x400];
	struct mVLBlockHeader header = { 0 };
	STORE_32LE(mVL_BLOCK_DATA, 0, &header.blockType);

	STORE_32LE(context->activeChannel, 0, &header.channelId);
	STORE_32LE(mVL_FLAG_BLOCK_COMPRESSED, 0, &header.flags);

	uint8_t compressBuffer[0x800];
	z_stream zstr;
	zstr.zalloc = Z_NULL;
	zstr.zfree = Z_NULL;
	zstr.opaque = Z_NULL;
	zstr.avail_in = 0;
	zstr.avail_out = sizeof(compressBuffer);
	zstr.next_out = (Bytef*) compressBuffer;
	if (deflateInit(&zstr, 9) != Z_OK) {
		return;
	}

	struct VFile* vfm = VFileMemChunk(NULL, 0);

	while (CircleBufferSize(buffer)) {
		size_t read = CircleBufferRead(buffer, writeBuffer, sizeof(writeBuffer));
		zstr.avail_in = read;
		zstr.next_in = (Bytef*) writeBuffer;
		while (zstr.avail_in) {
			if (deflate(&zstr, Z_NO_FLUSH) == Z_STREAM_ERROR) {
				break;
			}
			vfm->write(vfm, compressBuffer, sizeof(compressBuffer) - zstr.avail_out);
			zstr.avail_out = sizeof(compressBuffer);
			zstr.next_out = (Bytef*) compressBuffer;
		}
	}

	do {
		zstr.avail_out = sizeof(compressBuffer);
		zstr.next_out = (Bytef*) compressBuffer;
		zstr.avail_in = 0;
		int ret = deflate(&zstr, Z_FINISH);
		if (ret == Z_STREAM_ERROR) {
			break;
		}
		vfm->write(vfm, compressBuffer, sizeof(compressBuffer) - zstr.avail_out);
	} while (sizeof(compressBuffer) - zstr.avail_out);

	size_t size = vfm->size(vfm);
	STORE_32LE(size, 0, &header.length);
	context->backing->write(context->backing, &header, sizeof(header));
	void* vfmm = vfm->map(vfm, size, MAP_READ);
	context->backing->write(context->backing, vfmm, size);
	vfm->unmap(vfm, vfmm, size);
	vfm->close(vfm);
}
#endif

static void _flushBuffer(struct mVideoLogContext* context) {
#ifdef USE_ZLIB
	// TODO: Make optional
	_flushBufferCompressed(context);
	return;
#endif

	struct CircleBuffer* buffer = &context->channels[context->activeChannel].buffer;
	if (!CircleBufferSize(buffer)) {
		return;
	}
	struct mVLBlockHeader header = { 0 };
	STORE_32LE(mVL_BLOCK_DATA, 0, &header.blockType);
	STORE_32LE(CircleBufferSize(buffer), 0, &header.length);
	STORE_32LE(context->activeChannel, 0, &header.channelId);

	context->backing->write(context->backing, &header, sizeof(header));

	uint8_t writeBuffer[0x800];
	while (CircleBufferSize(buffer)) {
		size_t read = CircleBufferRead(buffer, writeBuffer, sizeof(writeBuffer));
		context->backing->write(context->backing, writeBuffer, read);
	}
}

void mVideoLogContextDestroy(struct mCore* core, struct mVideoLogContext* context) {
	if (context->write) {
		_flushBuffer(context);

		struct mVLBlockHeader header = { 0 };
		STORE_32LE(mVL_BLOCK_FOOTER, 0, &header.blockType);
		context->backing->write(context->backing, &header, sizeof(header));
	}

	if (core) {
		core->endVideoLog(core);
	}
	if (context->initialState) {
		mappedMemoryFree(context->initialState, context->initialStateSize);
	}
	free(context);
}

void mVideoLogContextRewind(struct mVideoLogContext* context, struct mCore* core) {
	_readHeader(context);
	if (core) {
		core->loadState(core, context->initialState);
	}

	off_t pointer = context->backing->seek(context->backing, 0, SEEK_CUR);

	size_t i;
	for (i = 0; i < context->nChannels; ++i) {
		CircleBufferClear(&context->channels[i].buffer);
		context->channels[i].bufferRemaining = 0;
		context->channels[i].currentPointer = pointer;
	}
}

void* mVideoLogContextInitialState(struct mVideoLogContext* context, size_t* size) {
	if (size) {
		*size = context->initialStateSize;
	}
	return context->initialState;
}

int mVideoLoggerAddChannel(struct mVideoLogContext* context) {
	if (context->nChannels >= mVL_MAX_CHANNELS) {
		return -1;
	}
	int chid = context->nChannels;
	++context->nChannels;
	context->channels[chid].p = context;
	CircleBufferInit(&context->channels[chid].buffer, BUFFER_BASE_SIZE);
	return chid;
}

#ifdef USE_ZLIB
static size_t _readBufferCompressed(struct VFile* vf, struct mVideoLogChannel* channel, size_t length) {
	uint8_t fbuffer[0x400];
	uint8_t zbuffer[0x800];
	size_t read = 0;

	channel->inflateStream.avail_in = 0;
	while (length) {
		size_t thisWrite = sizeof(zbuffer);
		if (thisWrite > length) {
			thisWrite = length;
		}

		size_t thisRead = 0;
		if (channel->inflating && channel->inflateStream.avail_in) {
			channel->inflateStream.next_out = zbuffer;
			channel->inflateStream.avail_out = thisWrite;
			thisRead = channel->inflateStream.avail_in;
		} else if (channel->bufferRemaining) {
			thisRead = sizeof(fbuffer);
			if (thisRead > channel->bufferRemaining) {
				thisRead = channel->bufferRemaining;
			}

			thisRead = vf->read(vf, fbuffer, thisRead);
			if (thisRead <= 0) {
				break;
			}

			channel->inflateStream.next_in = fbuffer;
			channel->inflateStream.avail_in = thisRead;
			channel->inflateStream.next_out = zbuffer;
			channel->inflateStream.avail_out = thisWrite;

			if (!channel->inflating) {
				if (inflateInit(&channel->inflateStream) != Z_OK) {
					break;
				}
				channel->inflating = true;
			}
		} else {
			channel->inflateStream.next_in = Z_NULL;
			channel->inflateStream.avail_in = 0;
			channel->inflateStream.next_out = zbuffer;
			channel->inflateStream.avail_out = thisWrite;
		}

		int ret = inflate(&channel->inflateStream, Z_NO_FLUSH);

		if (channel->inflateStream.next_in != Z_NULL) {
			thisRead -= channel->inflateStream.avail_in;
			channel->currentPointer += thisRead;
			channel->bufferRemaining -= thisRead;
		}

		if (ret != Z_OK) {
			inflateEnd(&channel->inflateStream);
			channel->inflating = false;
			if (ret != Z_STREAM_END) {
				break;
			}
		}

		thisWrite = CircleBufferWrite(&channel->buffer, zbuffer, thisWrite - channel->inflateStream.avail_out);
		length -= thisWrite;
		read += thisWrite;

		if (!channel->inflating) {
			break;
		}
	}
	return read;
}
#endif

static void _readBuffer(struct VFile* vf, struct mVideoLogChannel* channel, size_t length) {
	uint8_t buffer[0x800];
	while (length) {
		size_t thisRead = sizeof(buffer);
		if (thisRead > length) {
			thisRead = length;
		}
		thisRead = vf->read(vf, buffer, thisRead);
		if (thisRead <= 0) {
			return;
		}
		size_t thisWrite = CircleBufferWrite(&channel->buffer, buffer, thisRead);
		length -= thisWrite;
		channel->bufferRemaining -= thisWrite;
		channel->currentPointer += thisWrite;
		if (thisWrite < thisRead) {
			break;
		}
	}
}

static bool _fillBuffer(struct mVideoLogContext* context, size_t channelId, size_t length) {
	struct mVideoLogChannel* channel = &context->channels[channelId];
	context->backing->seek(context->backing, channel->currentPointer, SEEK_SET);
	struct mVLBlockHeader header;
	while (length) {
		size_t bufferRemaining = channel->bufferRemaining;
		if (bufferRemaining) {
#ifdef USE_ZLIB
			if (channel->inflating) {
				length -= _readBufferCompressed(context->backing, channel, length);
				continue;
			}
#endif
			if (bufferRemaining > length) {
				bufferRemaining = length;
			}

			_readBuffer(context->backing, channel, bufferRemaining);
			length -= bufferRemaining;
			continue;
		}

		if (!_readBlockHeader(context, &header)) {
			return false;
		}
		if (header.blockType == mVL_BLOCK_FOOTER) {
			return false;
		}
		if (header.channelId != channelId || header.blockType != mVL_BLOCK_DATA) {
			context->backing->seek(context->backing, header.length, SEEK_CUR);
			continue;
		}
		channel->currentPointer = context->backing->seek(context->backing, 0, SEEK_CUR);
		if (!header.length) {
			continue;
		}
		channel->bufferRemaining = header.length;

		if (header.flags & mVL_FLAG_BLOCK_COMPRESSED) {
#ifdef USE_ZLIB
			length -= _readBufferCompressed(context->backing, channel, length);
#else
			return false;
#endif
		}
	}
	return true;
}

static ssize_t mVideoLoggerReadChannel(struct mVideoLogChannel* channel, void* data, size_t length) {
	struct mVideoLogContext* context = channel->p;
	unsigned channelId = channel - context->channels;
	if (channelId >= mVL_MAX_CHANNELS) {
		return 0;
	}
	if (CircleBufferSize(&channel->buffer) >= length) {
		return CircleBufferRead(&channel->buffer, data, length);
	}
	ssize_t size = 0;
	if (CircleBufferSize(&channel->buffer)) {
		size = CircleBufferRead(&channel->buffer, data, CircleBufferSize(&channel->buffer));
		if (size <= 0) {
			return size;
		}
		data = (uint8_t*) data + size;
		length -= size;
	}
	if (!_fillBuffer(context, channelId, BUFFER_BASE_SIZE)) {
		return size;
	}
	size += CircleBufferRead(&channel->buffer, data, length);
	return size;
}

static ssize_t mVideoLoggerWriteChannel(struct mVideoLogChannel* channel, const void* data, size_t length) {
	struct mVideoLogContext* context = channel->p;
	unsigned channelId = channel - context->channels;
	if (channelId >= mVL_MAX_CHANNELS) {
		return 0;
	}
	if (channelId != context->activeChannel) {
		_flushBuffer(context);
		context->activeChannel = channelId;
	}
	if (CircleBufferCapacity(&channel->buffer) - CircleBufferSize(&channel->buffer) < length) {
		_flushBuffer(context);
		if (CircleBufferCapacity(&channel->buffer) < length) {
			CircleBufferDeinit(&channel->buffer);
			CircleBufferInit(&channel->buffer, toPow2(length << 1));
		}
	}

	ssize_t read = CircleBufferWrite(&channel->buffer, data, length);
	if (CircleBufferCapacity(&channel->buffer) == CircleBufferSize(&channel->buffer)) {
		_flushBuffer(context);
	}
	return read;
}

struct mCore* mVideoLogCoreFind(struct VFile* vf) {
	if (!vf) {
		return NULL;
	}
	struct mVideoLogHeader header = { { 0 } };
	vf->seek(vf, 0, SEEK_SET);
	ssize_t read = vf->read(vf, &header, sizeof(header));
	if (read != sizeof(header)) {
		return NULL;
	}
	if (memcmp(header.magic, mVL_MAGIC, sizeof(header.magic)) != 0) {
		return NULL;
	}
	enum mPlatform platform;
	LOAD_32LE(platform, 0, &header.platform);

	const struct mVLDescriptor* descriptor;
	for (descriptor = &_descriptors[0]; descriptor->platform != PLATFORM_NONE; ++descriptor) {
		if (platform == descriptor->platform) {
			break;
		}
	}
	struct mCore* core = NULL;
	if (descriptor->open) {
		core = descriptor->open();
	}
	return core;
}

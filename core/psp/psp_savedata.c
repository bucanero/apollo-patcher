/*
 * psp_savedata - the PSP's own savedata encryption. See psp_savedata.h.
 *
 * Vendored into apollo-patcher from bucanero/apollo-psp @ 17cb5ea,
 * source/psp_decrypter.c, which is itself jpcsp's CryptoEngine by way of
 * cielavenir's psp-savedata-endecrypter. The crypto below -- ScrambleSD
 * through UpdateSavedataHashes -- is upstream's, kept as close to verbatim as
 * it can be so the two stay diffable; it is proven byte-exact against real
 * console saves and is not the part to get clever with.
 *
 * What changed is the layer around it. Upstream reads and writes files:
 *
 *     int psp_DecryptSavedata(const char *fpath, const char *fname, uint8_t *key);
 *     int psp_EncryptSavedata(const char *fpath, const char *fname, uint8_t *key);
 *
 * -- where `fpath` is a directory with a trailing separator, `fname` is a full
 * path in one function and a bare name joined to `fpath` in the other, and the
 * result is written back over the input. None of that survives contact with a
 * browser tab, so this file is buffers in, buffers out, and the PARAM.SFO
 * parsing is bounds-checked against the length it was given rather than
 * trusting the offsets inside the file.
 *
 * The four deliberate divergences are each commented where they are: mode
 * selection is now an error instead of silent garbage, the two scratch buffers
 * are calloc'd, the dead random-IV branch is gone, and the SFO walk uses the
 * file's own declared extent instead of a hardcoded 0xC60.
 *
 * Original credits, kept:
 *
 *     kirk-engine (C) draan / proxima
 *     jpcsp (C) jpcsp team, especially CryptoEngine by hykem
 *     ported by popsdeco (aka @cielavenir)
 *     acknowledgement: referred SED-PC to fix the hashing algorithm
 *
 * This file is part of jpcsp.
 *
 * Jpcsp is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Jpcsp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Jpcsp.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <dbglogger.h>

#include "kirk_engine.h"
#include "psp_savedata.h"

/* Same sink the rest of the engine logs through -- apollo_ctrl.c defines
 * dbglogger_log() and routes it to the front-end's log panel. */
#define LOG dbglogger_log

#define arraycopy(src,srcPos,dest,destPos,len) memmove((dest)+(destPos),(src)+(srcPos),(len))

/* The Fuse ID in effect. Only ever reaches the mode-4/6 PARAM.SFO hashes;
 * see the note in psp_savedata.h. */
static uint64_t g_fuse_id = KIRK_HOST_FUSE_ID;

void apsp_set_fuse_id(uint64_t fuse_id) { g_fuse_id = fuse_id; }
uint64_t apsp_get_fuse_id(void)         { return g_fuse_id; }

const char *apsp_strerror(int err)
{
    switch (err) {
    case APSP_OK:           return "OK";
    case APSP_ERR_SFO:      return "PARAM.SFO is missing, truncated or malformed";
    case APSP_ERR_NO_PARAM: return "PARAM.SFO carries no SAVEDATA_PARAMS";
    case APSP_ERR_NO_FILE:  return "that file is not listed in PARAM.SFO's SAVEDATA_FILE_LIST";
    case APSP_ERR_SIZE:     return "the save file is too short to be encrypted savedata";
    case APSP_ERR_MODE:     return "PARAM.SFO asks for an encryption mode this does not implement";
    case APSP_ERR_ARG:      return "missing argument";
    case APSP_ERR_MEM:      return "out of memory";
    case APSP_ERR_NO_KEY:   return "no game key for this save in the Apollo database";
    default:                return "unknown error";
    }
}

/* Upstream also carries sdHashKey1 = 40 E6 53 3F 05 11 3A 4E A1 4B DA D6 72 7C
 * 53 4C, which nothing reads: mode 1 is the unkeyed path and mixes in no hash
 * key of its own. Dropped rather than left to warn on every build. */
static uint8_t sdHashKey2[] = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
static uint8_t sdHashKey3[] = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
static uint8_t sdHashKey4[] = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};
static uint8_t sdHashKey5[] = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
static uint8_t sdHashKey6[] = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
static uint8_t sdHashKey7[] = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};

typedef struct{
		int mode;
		uint8_t key[16];
		uint8_t pad[16];
		unsigned int padSize;
} _SD_Ctx1, *SD_Ctx1;

typedef struct{
		int mode;
		int unk;
		uint8_t buf[16];
} _SD_Ctx2, *SD_Ctx2;

static int isNullKey(uint8_t* key) {
	if (key != NULL) {
		for (int i=0; i < 0x10; i++) {
			if (key[i] != (uint8_t) 0) {
				return 0;
			}
		}
	}
	return 1;
}

static void xorKey(uint8_t* dest, int dest_offset, const uint8_t* src, int src_offset, int size) {
	for (int i=0; i < size; i++) {
		dest[dest_offset + i] = (uint8_t) (dest[dest_offset + i] ^ src[src_offset + i]);
	}
}

static void ScrambleSD(uint8_t *buf, int size, int seed, int cbc, int kirk_code) {
	KIRK_AES128CBC_HEADER *header = (KIRK_AES128CBC_HEADER*)buf;

	// Set CBC mode.
	header->mode = cbc;

	// Set unkown parameters to 0.
	header->unk_4 = 0;
	header->unk_8 = 0;

	// Set the the key seed to seed.
	header->keyseed = seed;

	// Set the the data size to size.
	header->data_size = size;

	sceUtilsBufferCopyWithRange(buf, size, buf, size, kirk_code);
	if(kirk_code==KIRK_CMD_ENCRYPT_IV_0)
		memmove(buf,buf+20,size);
}

static int getModeSeed(int mode) {
	int seed;
	switch (mode) {
		case 0x6:
			seed = 0x11;
			break;
		case 0x4:
			seed = 0xD;
			break;
		case 0x2:
			seed = 0x5;
			break;
		case 0x1:
			seed = 0x3;
			break;
		case 0x3:
			seed = 0xC;
			break;
		default:
			seed = 0x10;
			break;
	}
	return seed;
}

/*
 * Which SD mode SAVEDATA_PARAMS[0] selects, or -1 when it names none.
 *
 * Hoisted out of Decrypt/EncryptSavedata, which had this inline and, in the
 * default case, logged an error and then carried on with `declared` -- a value
 * that is never 1, 3 or 5, so every downstream branch fell through and the
 * caller got silent garbage back. Now it is an error both directions.
 */
static int sdModeFor(const uint8_t *key, int declared)
{
	if (isNullKey((uint8_t *)key))
		return 1;
	if (declared & 0x20)
		return 3;
	if (declared & 0x40)
		return 5;   // firmware 2.5.2 and later
	return -1;
}

static void cryptMember(SD_Ctx2 ctx, uint8_t* data, int data_offset, int length) {
	int finalSeed;
	uint8_t *dataBuf = malloc(length + 0x14);
	uint8_t keyBuf[0x10 + 0x10];
	uint8_t hashBuf[0x10];

	memset(dataBuf,0,length + 0x14);
	memset(keyBuf,0,sizeof(keyBuf));
	memset(hashBuf,0,sizeof(hashBuf));

	// Copy the hash stored by hleSdCreateList.
	arraycopy(ctx->buf, 0, dataBuf, 0x14, 0x10);

	if (ctx->mode == 0x1) {
		// Decryption mode 0x01: decrypt the hash directly with KIRK CMD7.
		ScrambleSD(dataBuf, 0x10, 0x4, KIRK_MODE_DECRYPT_CBC, KIRK_CMD_DECRYPT_IV_0);
		finalSeed = 0x53;
	} else if (ctx->mode == 0x3) {
		// Decryption mode 0x03: XOR the hash with SD keys and decrypt with KIRK CMD7.
		xorKey(dataBuf, 0x14, sdHashKey4, 0, 0x10);
		ScrambleSD(dataBuf, 0x10, 0xE, KIRK_MODE_DECRYPT_CBC, KIRK_CMD_DECRYPT_IV_0);
		xorKey(dataBuf, 0, sdHashKey3, 0, 0x10);
		finalSeed = 0x57;
	} else if (ctx->mode == 0x5) {
		// Decryption mode 0x05: XOR the hash with new SD keys and decrypt with KIRK CMD7.
		xorKey(dataBuf, 0x14, sdHashKey7, 0, 0x10);
		ScrambleSD(dataBuf, 0x10, 0x12, KIRK_MODE_DECRYPT_CBC, KIRK_CMD_DECRYPT_IV_0);
		xorKey(dataBuf, 0, sdHashKey6, 0, 0x10);
		finalSeed = 0x64;
	} else {
		// unsupported mode
	}

	// Store the calculated key.
	arraycopy(dataBuf, 0, keyBuf, 0x10, 0x10);

	// Apply extra padding if ctx.unk is not 1.
	if (ctx->unk != 0x1) {
		arraycopy(keyBuf, 0x10, keyBuf, 0, 0xC);
		keyBuf[0xC] = (uint8_t) ((ctx->unk - 1) & 0xFF);
		keyBuf[0xD] = (uint8_t) (((ctx->unk - 1) >> 8) & 0xFF);
		keyBuf[0xE] = (uint8_t) (((ctx->unk - 1) >> 16) & 0xFF);
		keyBuf[0xF] = (uint8_t) (((ctx->unk - 1) >> 24) & 0xFF);
	}

	// Copy the first 0xC bytes of the obtained key and replicate them
	// across a new list buffer. As a terminator, add the ctx1.seed parameter's
	// 4 bytes (endian swapped) to achieve a full numbered list.
	for (int i=0x14; i < (length + 0x14); i += 0x10) {
		arraycopy(keyBuf, 0x10, dataBuf, i, 0xC);
		dataBuf[i + 0xC] = (uint8_t) (ctx->unk & 0xFF);
		dataBuf[i + 0xD] = (uint8_t) ((ctx->unk >> 8) & 0xFF);
		dataBuf[i + 0xE] = (uint8_t) ((ctx->unk >> 16) & 0xFF);
		dataBuf[i + 0xF] = (uint8_t) ((ctx->unk >> 24) & 0xFF);
		ctx->unk++;
	}

	arraycopy(dataBuf, length + 0x04, hashBuf, 0, 0x10);

	ScrambleSD(dataBuf, length, finalSeed, KIRK_MODE_DECRYPT_CBC, KIRK_CMD_DECRYPT_IV_0);

	// XOR the first 16-bytes of data with the saved key to generate a new hash.
	xorKey(dataBuf, 0, keyBuf, 0, 0x10);

	// Copy back the last hash from the list to the first half of keyBuf.
	arraycopy(hashBuf, 0, keyBuf, 0, 0x10);

	// Finally, XOR the full list with the given data.
	xorKey(data, data_offset, dataBuf, 0, length);
	free(dataBuf);
}

/*
 * sceSd - chnnlsv.prx
 */
static int hleSdSetIndex(SD_Ctx1 ctx, int encMode) {
	// Set all parameters to 0 and assign the encMode.
	ctx->mode = encMode;
	return 0;
}

static int hleSdCreateList(SD_Ctx2 ctx, int encMode, int genMode, uint8_t* data, uint8_t* key) {
	// If the key is not a 16-uint8_t key, return an error.
	//if (key.length < 0x10) {
	//	return -1;
	//}

	// Set the mode and the unknown parameters.
	ctx->mode = encMode;
	ctx->unk = 0x1;

	// Key generator mode 0x1 (encryption): use an encrypted pseudo random number before XORing the data with the given key.
	if (genMode == 0x1) {
		uint8_t header[0x25];
		uint8_t seed[0x14];

		// Generate SHA-1 to act as seed for encryption.
		sceUtilsBufferCopyWithRange(seed, 0x14, NULL, 0, KIRK_CMD_PRNG);
		
		// Propagate SHA-1 in kirk header.
		arraycopy(seed, 0, header, 0, 0x10);
		arraycopy(seed, 0, header, 0x14, 0x10);

		// Encryption mode 0x1: encrypt with KIRK CMD4 and XOR with the given key.
		if (ctx->mode == 0x1) {
			ScrambleSD(header, 0x10, 0x4, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
			arraycopy(header, 0, ctx->buf, 0, 0x10);
			arraycopy(header, 0, data, 0, 0x10);
			// If the key is not null, XOR the hash with it.
			if (!isNullKey(key)) {
				xorKey(ctx->buf, 0, key, 0, 0x10);
			}
			return 0;
		} else if (ctx->mode == 0x3) { // Encryption mode 0x3: XOR with SD keys, encrypt with KIRK CMD4 and XOR with the given key.
			xorKey(header, 0x14, sdHashKey3, 0, 0x10);
			ScrambleSD(header, 0x10, 0xE, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
			xorKey(header, 0, sdHashKey4, 0, 0x10);
			arraycopy(header, 0, ctx->buf, 0, 0x10);
			arraycopy(header, 0, data, 0, 0x10);
			// If the key is not null, XOR the hash with it.
			if (!isNullKey(key)) {
				xorKey(ctx->buf, 0, key, 0, 0x10);
			}
			return 0;
		} else if (ctx->mode == 0x5) { // Encryption mode 0x5: XOR with new SD keys, encrypt with KIRK CMD4 and XOR with the given key.
			xorKey(header, 0x14, sdHashKey6, 0, 0x10);
			ScrambleSD(header, 0x10, 0x12, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
			xorKey(header, 0, sdHashKey7, 0, 0x10);
			arraycopy(header, 0, ctx->buf, 0, 0x10);
			arraycopy(header, 0, data, 0, 0x10);
			// If the key is not null, XOR the hash with it.
			if (!isNullKey(key)) {
				xorKey(ctx->buf, 0, key, 0, 0x10);
			}
			return 0;
		} else {
			// unsupported mode
			return (-1);
		}
	} else if (genMode == 0x2) { // Key generator mode 0x02 (decryption): directly XOR the data with the given key.
		// Grab the data hash (first 16-bytes).
		arraycopy(data, 0, ctx->buf, 0, 0x10);
		// If the key is not null, XOR the hash with it.
		if (!isNullKey(key)) {
			xorKey(ctx->buf, 0, key, 0, 0x10);
		}
		return 0;
	} else {
		// Invalid mode.
		return -1;
	}
}

static int hleSdRemoveValue(SD_Ctx1 ctx, uint8_t *data, int length) {
	if (ctx->padSize > 0x10 || (length < 0)) {
		// Invalid key or length.
		return -1;
	} else if (((ctx->padSize + length) <= 0x10)) {
		// The key hasn't been set yet.
		// Extract the hash from the data and set it as the key.
		arraycopy(data, 0, ctx->pad, ctx->padSize, length);
		ctx->padSize += length;
		return 0;
	} else {
		// Calculate the seed.
		int seed = getModeSeed(ctx->mode);

		// Setup the buffers.
		uint8_t *scrambleBuf = malloc((length + ctx->padSize) + 0x14);

		// Copy the previous key to the buffer.
		arraycopy(ctx->pad, 0, scrambleBuf, 0x14, ctx->padSize);

		// Calculate new key length.
		int kLen = ctx->padSize;

		ctx->padSize += length;
		ctx->padSize &= 0x0F;
		if (ctx->padSize == 0) {
			ctx->padSize = 0x10;
		}

		// Calculate new data length.
		length -= ctx->padSize;

		// Copy data's footer to make a new key.
		arraycopy(data, length, ctx->pad, 0, ctx->padSize);

		// Process the encryption in 0x800 blocks.
		int blockSize = 0;
		int dataOffset = 0;

		while (length > 0) {
			blockSize = (length + kLen >= 0x800) ? 0x800 : length + kLen;

			arraycopy(data, dataOffset, scrambleBuf, 0x14 + kLen, blockSize - kLen);

			// Encrypt with KIRK CMD 4 and XOR with result.
			xorKey(scrambleBuf, 0x14, ctx->key, 0, 0x10);
			ScrambleSD(scrambleBuf, blockSize, seed, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
			arraycopy(scrambleBuf, (blockSize + 0x4) - 0x14, ctx->key, 0, 0x10);

			// Adjust data length, data offset and reset any key length.
			length -= (blockSize - kLen);
			dataOffset += (blockSize - kLen);
			kLen = 0;
		}
		free(scrambleBuf);
		return 0;
	}
}

static int hleSdGetLastIndex(SD_Ctx1 ctx, uint8_t *hash, uint8_t *key) {
	int i;
	if (ctx->padSize > 0x10) {
		// Invalid key length.
		return -1;
	}

	// Setup the buffers.
	uint8_t scrambleEmptyBuf[0x10 + 0x14];
	uint8_t keyBuf[0x10];
	uint8_t scrambleKeyBuf[0x10 + 0x14];
	uint8_t resultBuf[0x10];
	uint8_t scrambleResultBuf[0x10 + 0x14];
	uint8_t scrambleResultKeyBuf[0x10 + 0x14];

	memset(scrambleEmptyBuf,0,sizeof(scrambleEmptyBuf));
	memset(keyBuf,0,sizeof(keyBuf));
	memset(scrambleKeyBuf,0,sizeof(scrambleKeyBuf));
	memset(resultBuf,0,sizeof(resultBuf));
	memset(scrambleResultBuf,0,sizeof(scrambleResultBuf));
	memset(scrambleResultKeyBuf,0,sizeof(scrambleResultKeyBuf));

	// Calculate the seed.
	int seed = getModeSeed(ctx->mode);

	// Encrypt an empty buffer with KIRK CMD 4.
	ScrambleSD(scrambleEmptyBuf, 0x10, seed, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
	arraycopy(scrambleEmptyBuf, 0, keyBuf, 0, 0x10);

	// Apply custom padding management.
	uint8_t b = ((keyBuf[0] & (uint8_t) 0x80) != 0) ? (uint8_t) 0x87 : 0;
	for (i = 0; i < 0xF; i++) {
		keyBuf[i] = (uint8_t) ((keyBuf[i] << 1) | ((keyBuf[i + 1] >> 7) & 0x01));
	}
	keyBuf[0xF] = (uint8_t) ((keyBuf[0xF] << 1) ^ b);

	if (ctx->padSize < 0x10) {
		uint8_t bb = ((keyBuf[0] & (uint8_t) 0x80) != 0) ? (uint8_t) 0x87 : 0;
		for (i = 0; i < 0xF; i++) {
			keyBuf[i] = (uint8_t) ((keyBuf[i] << 1) | ((keyBuf[i + 1] >> 7) & 0x01));
		}
		keyBuf[0xF] = (uint8_t) ((keyBuf[0xF] << 1) ^ bb);

		ctx->pad[ctx->padSize] = (uint8_t) 0x80;
		if ((ctx->padSize + 1) < 0x10) {
			for (i = 0; i < (0x10 - ctx->padSize - 1); i++) {
				ctx->pad[ctx->padSize + 1 + i] = 0;
			}
		}
	}

	// XOR previous key with new one.
	xorKey(ctx->pad, 0, keyBuf, 0, 0x10);

	arraycopy(ctx->pad, 0, scrambleKeyBuf, 0x14, 0x10);
	arraycopy(ctx->key, 0, resultBuf, 0, 0x10);

	// Encrypt with KIRK CMD 4 and XOR with result.
	xorKey(scrambleKeyBuf, 0x14, resultBuf, 0, 0x10);
	ScrambleSD(scrambleKeyBuf, 0x10, seed, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
	arraycopy(scrambleKeyBuf, (0x10 + 0x4) - 0x14, resultBuf, 0, 0x10);

	// If ctx.mode is new mode 0x5 or 0x6, XOR with the new hash key 5, else, XOR with hash key 2.
	if ((ctx->mode == 0x5) || (ctx->mode == 0x6)) {
		xorKey(resultBuf, 0, sdHashKey5, 0, 0x10);
	} else if ((ctx->mode == 0x3) || (ctx->mode == 0x4)) {
		xorKey(resultBuf, 0, sdHashKey2, 0, 0x10);
	}

	// If mode is 2, 4 or 6, encrypt again with KIRK CMD 5 and then KIRK CMD 4.
	if ((ctx->mode == 0x2) || (ctx->mode == 0x4) || (ctx->mode == 0x6)) {
		// Copy the result buffer into the data buffer.
		arraycopy(resultBuf, 0, scrambleResultBuf, 0x14, 0x10);

		// Encrypt with KIRK CMD 5 (seed is always 0x100).
		ScrambleSD(scrambleResultBuf, 0x10, 0x100, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_FUSE);

		// Encrypt again with KIRK CMD 4.
		ScrambleSD(scrambleResultBuf, 0x10, seed, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
		arraycopy(scrambleResultBuf, 0, resultBuf, 0, 0x10);
	}

	// XOR with the supplied key and encrypt with KIRK CMD 4.
	if (key != NULL) {
		xorKey(resultBuf, 0, key, 0, 0x10);
		arraycopy(resultBuf, 0, scrambleResultKeyBuf, 0x14, 0x10);
		ScrambleSD(scrambleResultKeyBuf, 0x10, seed, KIRK_MODE_ENCRYPT_CBC, KIRK_CMD_ENCRYPT_IV_0);
		arraycopy(scrambleResultKeyBuf, 0, resultBuf, 0, 0x10);
	}

	// Copy back the generated hash.
	arraycopy(resultBuf, 0, hash, 0, 0x10);

	// Clear the context fields.
	memset(ctx,0,sizeof(_SD_Ctx1));

	return 0;
}

static int hleSdSetMember(SD_Ctx2 ctx, uint8_t* data, int length) {
	if (length <= 0) {
		return -1;
	}

	// Parse the data in 0x800 blocks first.
	int index = 0;
	if (length >= 0x800) {
		for (index = 0; length >= 0x800; index += 0x800) {
			cryptMember(ctx, data, index, 0x800);
			length -= 0x800;
		}
	}

	// Finally parse the rest of the data.
	if (length)
		cryptMember(ctx, data, index, length);

	return 0;
}

static int DecryptSavedata(uint8_t *buf, int size, uint8_t *key, int sdDecMode) {
	// Initialize the context structs.
	_SD_Ctx1 ctx1;
	_SD_Ctx2 ctx2;
	memset(&ctx1,0,sizeof(ctx1));
	memset(&ctx2,0,sizeof(ctx2));

	// Set the decryption mode.
	int mode = sdModeFor(key, sdDecMode);
	if (mode < 0) {
		LOG("Error: unsupported DecMode %X", sdDecMode);
		return APSP_ERR_MODE;
	}
	sdDecMode = mode;

	// Setup the buffers. calloc, not malloc: the aligned tail past
	// size - 0x10 is never written before it is read back. Nothing downstream
	// of it reaches the output -- hleSdGetLastIndex, the one consumer that
	// would carry it, is not called on this path -- so upstream's malloc is
	// harmless, but reading uninitialised memory at all trips every sanitiser
	// and makes a clean run unverifiable.
	int alignedSize = ((size + 0xF) >> 4) << 4;
	uint8_t *tmpbuf = calloc(1, alignedSize);
	if (!tmpbuf)
		return APSP_ERR_MEM;

	// Perform the decryption.
	hleSdSetIndex(&ctx1, sdDecMode);
	hleSdCreateList(&ctx2, sdDecMode, 2, buf, key);
	hleSdRemoveValue(&ctx1, buf, 0x10);

	arraycopy(buf, 0x10, tmpbuf, 0, size - 0x10);
	hleSdRemoveValue(&ctx1, tmpbuf, alignedSize);
	hleSdSetMember(&ctx2, tmpbuf, alignedSize);

	// Clear context 2.
	memset(&ctx2,0,sizeof(_SD_Ctx2));

	// Generate a file hash for this data.
	//hleSdGetLastIndex(&ctx1, hash, key);

	// Copy back the data.
	arraycopy(tmpbuf, 0, buf, 0, size - 0x10);
	free(tmpbuf);
	return APSP_OK;
}

static int EncryptSavedata(uint8_t* buf, int size, uint8_t *key, uint8_t *hash, int sdEncMode) {
	// Initialize the context structs.
	uint8_t iv[16] = "bucanero.com.ar";
	_SD_Ctx1 ctx1;
	_SD_Ctx2 ctx2;
	memset(&ctx1,0,sizeof(ctx1));
	memset(&ctx2,0,sizeof(ctx2));

	// Set the encryption mode.
	int mode = sdModeFor(key, sdEncMode);
	if (mode < 0) {
		LOG("Error: unsupported EncMode %X", sdEncMode);
		return APSP_ERR_MODE;
	}
	sdEncMode = mode;

	// Setup the buffers.
	int alignedSize = ((size + 0xF) >> 4) << 4;
	uint8_t header[0x10];
	uint8_t *tmpbuf = calloc(1, alignedSize);
	if (!tmpbuf)
		return APSP_ERR_MEM;

	memset(header,0,sizeof(header));

	// Copy the plain data to tmpbuf.
	arraycopy(buf, 0, tmpbuf, 0, size);

	// The encryption IV (first 0x10 bytes).
	//
	// A console picks this at random (hleSdCreateList genMode 1, off KIRK's
	// PRNG). Apollo uses a fixed one instead, which is why re-encrypting a
	// save does not reproduce the ciphertext the PSP wrote -- and why both
	// directions here are deterministic and can be pinned by a test. Upstream
	// keeps the random path behind `if(!iv)`, a test on the address of an
	// array, so it is never taken; dropped rather than left to read as live.
	ctx2.mode = sdEncMode;
	ctx2.unk = 0x1;
	memcpy(ctx2.buf,iv,0x10);
	if (!isNullKey(key)) {
		xorKey(ctx2.buf, 0, key, 0, 0x10);
	}
	memcpy(header,iv,0x10); //actually the same
	hleSdSetIndex(&ctx1, sdEncMode);
	hleSdRemoveValue(&ctx1, header, 0x10);
	hleSdSetMember(&ctx2, tmpbuf, alignedSize);

	// Clear extra bytes.
	for (int i = size; i < alignedSize; i++) {
		tmpbuf[i] = 0;
	}

	// Encrypt the data.
	hleSdRemoveValue(&ctx1, tmpbuf, alignedSize);

	// Copy back the encrypted data + IV.
	arraycopy(header, 0, buf, 0, 0x10);
	arraycopy(tmpbuf, 0, buf, 0x10, size);

	// Clear context 2.
	memset(&ctx2,0,sizeof(_SD_Ctx2));

	// Generate a file hash for this data.
	hleSdGetLastIndex(&ctx1, hash, key);
	free(tmpbuf);
	return APSP_OK;
}

static void GenerateSavedataHash(uint8_t *data, int size, int mode, uint8_t *hash) {
	_SD_Ctx1 ctx1;
	memset(&ctx1,0,sizeof(ctx1));

	// Generate a new hash using a key.
	hleSdSetIndex(&ctx1, mode);
	hleSdRemoveValue(&ctx1, data, size);
	if(hleSdGetLastIndex(&ctx1, hash, NULL)<0)
		memset(hash,1,0x10);
}

static void UpdateSavedataHashes(uint8_t* savedataParams, uint8_t* data, int size) {
	// Check for previous SAVEDATA_PARAMS data in the file.
	int mode = ((savedataParams[0] >> 4) & 0xF);
	int check_bit = ((savedataParams[0]) & 0xF);

	memset(savedataParams,0,0x80);

	if ((mode & 0x4) == 0x4) {
		// Generate a type 6 hash.
		GenerateSavedataHash(data, size, 6, savedataParams+0x20);
		savedataParams[0]|=0x41;

		// Generate a type 5 hash.
		GenerateSavedataHash(data, size, 5, savedataParams+0x70);
	} else if((mode & 0x2) == 0x2) {
		// Generate a type 4 hash.
		GenerateSavedataHash(data, size, 4, savedataParams+0x20);
		savedataParams[0]|=0x21;

		// Generate a type 3 hash.
		GenerateSavedataHash(data, size, 3, savedataParams+0x70);
	} else {
		// Generate a type 2 hash.
		GenerateSavedataHash(data, size, 2, savedataParams+0x20);
		savedataParams[0]|=0x01;
	}

	if ((check_bit & 0x1) == 0x1) {
		// Generate a type 1 hash.
		GenerateSavedataHash(data, size, 1, savedataParams+0x10);
	}
}

/* ------------------------------------------------------------------------ *
 * PARAM.SFO
 *
 * Layout, all little-endian:
 *
 *   0x00  u32  magic, "\0PSF"
 *   0x04  u32  version, 0x00000101
 *   0x08  u32  offset of the key table
 *   0x0C  u32  offset of the data table
 *   0x10  u32  entry count
 *   0x14  n x 16-byte index entries:
 *              0x00  u16  offset of this key's name within the key table
 *              0x02  u16  format
 *              0x04  u32  used length of the value
 *              0x08  u32  reserved length of the value
 *              0x0C  u32  offset of the value within the data table
 *
 * Every one of those offsets comes out of the file. Upstream followed them
 * unchecked, which is fine for a file off your own Memory Stick and an
 * out-of-bounds read for one a browser handed you, so each is checked against
 * the buffer's real length here before it is used.
 * ------------------------------------------------------------------------ */

#define SFO_MAGIC    0x46535000u
#define SFO_VERSION  0x00000101u

/* Not in the public enum: "the file parses, that key is simply not in it".
 * Callers map it to whichever of APSP_ERR_NO_PARAM / _NO_FILE they mean. */
#define SFO_NOT_FOUND (-100)

/* SAVEDATA_PARAMS is a fixed 0x80 block: the mode byte, then the two hashes
 * UpdateSavedataHashes() writes at +0x20 and +0x70. Anything shorter is not
 * one, whatever it claims. */
#define SFO_PARAMS_LEN 0x80

/* SAVEDATA_FILE_LIST is a packed array of these. Name first, NUL-padded but
 * not guaranteed NUL-terminated when it uses every byte, then the per-file
 * hash that encryption writes. */
#define FLIST_ENTRY_LEN 0x20
#define FLIST_HASH_OFF  0x0D

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* A located value. An OFFSET rather than a pointer, so the same lookup serves
 * the read-only calls and the two that write back into the SFO without a
 * const-cast anywhere. */
typedef struct {
    size_t   off;   /* absolute, into the caller's buffer */
    uint32_t len;   /* used bytes    */
    uint32_t max;   /* reserved bytes */
} sfo_val_t;

static int sfo_find(const uint8_t *sfo, size_t sfo_len, const char *name, sfo_val_t *out)
{
    uint32_t keys, data, count, i;

    if (!sfo || sfo_len < 0x14)
        return APSP_ERR_SFO;
    if (le32(sfo) != SFO_MAGIC || le32(sfo + 4) != SFO_VERSION)
        return APSP_ERR_SFO;

    keys  = le32(sfo + 0x08);
    data  = le32(sfo + 0x0C);
    count = le32(sfo + 0x10);

    if (keys > sfo_len || data > sfo_len)
        return APSP_ERR_SFO;
    /* Written as a division so a huge count cannot overflow the multiply. */
    if (count > (sfo_len - 0x14) / 0x10)
        return APSP_ERR_SFO;

    for (i = 0; i < count; i++) {
        const uint8_t *e = sfo + 0x14 + 0x10 * (size_t)i;
        uint32_t key_off  = le16(e);
        uint32_t val_len  = le32(e + 0x04);
        uint32_t val_max  = le32(e + 0x08);
        uint32_t val_off  = le32(e + 0x0C);
        const char *k;
        size_t room;

        if ((size_t)keys + key_off >= sfo_len)
            continue;
        /* The name has to actually terminate inside the key table, or strcmp
         * would run off the end of the buffer. */
        k    = (const char *)sfo + keys + key_off;
        room = sfo_len - (keys + key_off);
        if (!memchr(k, '\0', room) || strcmp(k, name) != 0)
            continue;

        /* Both subtractions are safe: data <= sfo_len was checked above, and
         * the first test leaves data + val_off <= sfo_len. */
        if (val_len > val_max)
            return APSP_ERR_SFO;
        if (val_off > sfo_len - data || val_max > sfo_len - data - val_off)
            return APSP_ERR_SFO;

        out->off = (size_t)data + val_off;
        out->len = val_len;
        out->max = val_max;
        return APSP_OK;
    }
    return SFO_NOT_FOUND;
}

/* SAVEDATA_PARAMS, which both directions need and which has to be long enough
 * to hold the hashes that get written back into it. */
static int sfo_params(const uint8_t *sfo, size_t sfo_len, sfo_val_t *out)
{
    int rc = sfo_find(sfo, sfo_len, "SAVEDATA_PARAMS", out);

    if (rc == SFO_NOT_FOUND)
        return APSP_ERR_NO_PARAM;
    if (rc != APSP_OK)
        return rc;
    if (out->max < SFO_PARAMS_LEN)
        return APSP_ERR_NO_PARAM;
    return APSP_OK;
}

/* A FILE_LIST entry's name, copied out rather than read in place: upstream
 * strcmp()s the entry directly, which walks into the hash that follows when a
 * name uses all APSP_NAME_LEN bytes. */
static void flist_name(const uint8_t *entry, char out[APSP_NAME_LEN + 1])
{
    memcpy(out, entry, APSP_NAME_LEN);
    out[APSP_NAME_LEN] = '\0';
}

/*
 * Walk SAVEDATA_FILE_LIST. `want` NULL enumerates and the n'th used entry's
 * offset comes back; `want` non-NULL looks that name up. Either way the whole
 * FLIST_ENTRY_LEN entry is required to be inside the value's declared extent
 * -- upstream stops at a hardcoded 0xC60 and only checks the entry's first
 * 0x0D bytes, so the hash it then writes at +0x0D could land past the end.
 *
 * Returns the number of used entries seen (>= 0), or a negative error. When
 * a match is found, *entry_off is its absolute offset.
 */
static int flist_walk(const uint8_t *sfo, size_t sfo_len, const char *want,
                      int index, size_t *entry_off)
{
    sfo_val_t v;
    uint32_t at;
    int used = 0;
    int rc = sfo_find(sfo, sfo_len, "SAVEDATA_FILE_LIST", &v);

    if (rc == SFO_NOT_FOUND)
        return APSP_ERR_NO_FILE;
    if (rc != APSP_OK)
        return rc;

    for (at = 0; at + FLIST_ENTRY_LEN <= v.max; at += FLIST_ENTRY_LEN) {
        const uint8_t *e = sfo + v.off + at;
        char name[APSP_NAME_LEN + 1];

        if (!e[0])
            continue;   /* an empty slot; real lists are packed, but not all */
        flist_name(e, name);

        if (want) {
            if (strcmp(name, want) == 0) {
                if (entry_off)
                    *entry_off = v.off + at;
                return 1;
            }
        } else if (used == index && entry_off) {
            *entry_off = v.off + at;
        }
        used++;
    }
    return want ? APSP_ERR_NO_FILE : used;
}

int apsp_sfo_valid(const uint8_t *sfo, size_t sfo_len)
{
    sfo_val_t v;

    return sfo_params(sfo, sfo_len, &v);
}

int apsp_sfo_mode(const uint8_t *sfo, size_t sfo_len)
{
    sfo_val_t v;
    int rc = sfo_params(sfo, sfo_len, &v);

    return rc == APSP_OK ? sfo[v.off] : rc;
}

int apsp_sfo_directory(const uint8_t *sfo, size_t sfo_len, char *out, size_t out_len)
{
    sfo_val_t v;
    int rc;
    size_t n;

    if (!out || !out_len)
        return APSP_ERR_ARG;
    *out = '\0';

    rc = sfo_find(sfo, sfo_len, "SAVEDATA_DIRECTORY", &v);
    if (rc == SFO_NOT_FOUND)
        return APSP_ERR_NO_PARAM;
    if (rc != APSP_OK)
        return rc;

    /* `len` counts the NUL the SFO stores; copy what fits and terminate. */
    n = v.len;
    if (n && sfo[v.off + n - 1] == '\0')
        n--;
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, sfo + v.off, n);
    out[n] = '\0';
    return APSP_OK;
}

int apsp_sfo_file_count(const uint8_t *sfo, size_t sfo_len)
{
    return flist_walk(sfo, sfo_len, NULL, -1, NULL);
}

int apsp_sfo_file_name(const uint8_t *sfo, size_t sfo_len, int index,
                       char *out, size_t out_len)
{
    const size_t NONE = (size_t)-1;
    size_t off = NONE;
    char name[APSP_NAME_LEN + 1];
    int count;

    if (!out || !out_len || index < 0)
        return APSP_ERR_ARG;
    *out = '\0';

    count = flist_walk(sfo, sfo_len, NULL, index, &off);
    if (count < 0)
        return count;
    if (index >= count || off == NONE)
        return APSP_ERR_NO_FILE;

    flist_name(sfo + off, name);
    if (strlen(name) >= out_len)
        return APSP_ERR_SIZE;
    strcpy(out, name);
    return APSP_OK;
}

/* ---- the game key ------------------------------------------------------- */

int apsp_key_from_buffer(const uint8_t *buf, size_t len, uint8_t key[APSP_KEY_LEN])
{
    if (!buf || !key)
        return APSP_ERR_ARG;

    switch (len) {
    case 0x10:                                  /* SGKeyDumper */
        memcpy(key, buf, APSP_KEY_LEN);
        return APSP_OK;
    case 0x600:                                 /* SGDeemer */
        memcpy(key, buf + 0x5DC, APSP_KEY_LEN);
        return APSP_OK;
    default:
        return APSP_ERR_SIZE;
    }
}

int apsp_key_from_hex(const char *hex, uint8_t key[APSP_KEY_LEN])
{
    int i;

    if (!hex || !key)
        return APSP_ERR_ARG;
    if (strlen(hex) != APSP_KEY_LEN * 2)
        return APSP_ERR_SIZE;

    for (i = 0; i < APSP_KEY_LEN * 2; i++) {
        char c = hex[i];
        int v;

        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return APSP_ERR_ARG;

        if (i & 1) key[i / 2] |= (uint8_t)v;
        else       key[i / 2]  = (uint8_t)(v << 4);
    }
    return APSP_OK;
}

int apsp_key_from_db(const char *text, size_t len, const char *directory,
                     uint8_t key[APSP_KEY_LEN], char *id_out, size_t id_cap)
{
    size_t at = 0, best = 0;
    uint8_t found[APSP_KEY_LEN];

    if (!text || !directory || !key)
        return APSP_ERR_ARG;
    if (id_out && id_cap)
        *id_out = '\0';

    while (at < len) {
        size_t eol = at, eq = 0, i;
        char hex[APSP_KEY_LEN * 2 + 1];

        while (eol < len && text[eol] != '\n' && text[eol] != '\r')
            eol++;

        /* Split on the first '=', skipping comments and anything shapeless. */
        for (i = at; i < eol; i++)
            if (text[i] == '=') { eq = i; break; }

        if (text[at] == ';' || !eq || eq == at)
            goto next;

        /* The id has to be a prefix of the directory, and a longer one than
         * whatever matched before. */
        {
            size_t idlen = eq - at;
            const char *id = text + at;

            if (idlen <= best || strlen(directory) < idlen)
                goto next;
            for (i = 0; i < idlen; i++) {
                char a = id[i], b = directory[i];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (b >= 'a' && b <= 'z') b -= 32;
                if (a != b)
                    goto next;
            }

            /* Only now is it worth parsing the key. Trailing junk after the
             * 32 digits is ignored, which is what makes a commented line like
             * `ID=KEY ; note` work. */
            if (eol - (eq + 1) < APSP_KEY_LEN * 2)
                goto next;
            memcpy(hex, text + eq + 1, APSP_KEY_LEN * 2);
            hex[APSP_KEY_LEN * 2] = '\0';
            if (apsp_key_from_hex(hex, found) != APSP_OK)
                goto next;

            best = idlen;
            memcpy(key, found, APSP_KEY_LEN);
            if (id_out && id_cap) {
                size_t n = idlen < id_cap - 1 ? idlen : id_cap - 1;
                memcpy(id_out, id, n);
                id_out[n] = '\0';
            }
        }

    next:
        at = eol;
        while (at < len && (text[at] == '\n' || text[at] == '\r'))
            at++;
    }

    return best ? APSP_OK : APSP_ERR_NO_KEY;
}

int apsp_key_is_null(const uint8_t key[APSP_KEY_LEN])
{
    return key ? isNullKey((uint8_t *)key) : 1;
}

/* ---- sizes -------------------------------------------------------------- */

size_t apsp_decrypted_size(size_t encrypted_len)
{
    return encrypted_len > APSP_HEADER_LEN ? encrypted_len - APSP_HEADER_LEN : 0;
}

size_t apsp_encrypted_size(size_t plain_len)
{
    return plain_len ? plain_len + APSP_HEADER_LEN : 0;
}

/* ---- decrypt / encrypt -------------------------------------------------- */

/* The crypto below takes `int` lengths throughout. No PSP save comes close,
 * but the check is a byte and the alternative is a negative size. */
static int size_fits(size_t n)
{
    return n <= (size_t)INT_MAX - 0x20;
}

int apsp_decrypt(const uint8_t *sfo, size_t sfo_len,
                 const uint8_t *in, size_t in_len,
                 const uint8_t key[APSP_KEY_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t plain = apsp_decrypted_size(in_len);
    uint8_t k[APSP_KEY_LEN];
    uint8_t *work;
    sfo_val_t params;
    int rc;

    if (!in || !out || !key)
        return APSP_ERR_ARG;
    if (!plain || !size_fits(in_len))
        return APSP_ERR_SIZE;
    if (out_cap < plain)
        return APSP_ERR_SIZE;

    /* Only the mode byte is wanted; the file's own name never comes into
     * decryption, so a save renamed on the way off the console still works. */
    rc = sfo_params(sfo, sfo_len, &params);
    if (rc != APSP_OK)
        return rc;

    kirk_init_fuse(g_fuse_id);

    /* Decryption runs in place over the whole ciphertext, IV included, and
     * leaves the plaintext in the first in_len - 0x10 bytes. The caller's
     * input stays untouched. */
    work = malloc(in_len);
    if (!work)
        return APSP_ERR_MEM;
    memcpy(work, in, in_len);
    memcpy(k, key, sizeof k);

    LOG("Decrypting %zu bytes, savedata mode %02X", in_len, sfo[params.off]);
    rc = DecryptSavedata(work, (int)in_len, k, sfo[params.off]);
    if (rc == APSP_OK) {
        memcpy(out, work, plain);
        if (out_len)
            *out_len = plain;
    }

    free(work);
    return rc;
}

int apsp_encrypt(uint8_t *sfo, size_t sfo_len, const char *name,
                 const uint8_t *in, size_t in_len,
                 const uint8_t key[APSP_KEY_LEN],
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t total = apsp_encrypted_size(in_len);
    size_t entry = 0;
    uint8_t k[APSP_KEY_LEN];
    uint8_t *work;
    sfo_val_t params;
    int rc;

    if (!sfo || !name || !in || !out || !key)
        return APSP_ERR_ARG;
    if (!total || !size_fits(total))
        return APSP_ERR_SIZE;
    if (out_cap < total)
        return APSP_ERR_SIZE;

    rc = sfo_params(sfo, sfo_len, &params);
    if (rc != APSP_OK)
        return rc;

    /* The file's own hash goes into its SAVEDATA_FILE_LIST entry, so a name
     * the SFO does not list has nowhere to put one. */
    rc = flist_walk(sfo, sfo_len, name, -1, &entry);
    if (rc < 0)
        return rc;

    kirk_init_fuse(g_fuse_id);

    work = calloc(1, total);
    if (!work)
        return APSP_ERR_MEM;
    memcpy(work, in, in_len);
    memcpy(k, key, sizeof k);

    LOG("Encrypting %s, %zu bytes, savedata mode %02X", name, in_len, sfo[params.off]);
    rc = EncryptSavedata(work, (int)in_len, k, sfo + entry + FLIST_HASH_OFF,
                         sfo[params.off]);
    if (rc == APSP_OK) {
        /* Order matters: the file's hash is now in the SFO, and the SFO-wide
         * hashes below are taken over a buffer that includes it. */
        UpdateSavedataHashes(sfo + params.off, sfo, sfo_len);
        memcpy(out, work, total);
        if (out_len)
            *out_len = total;
    }

    free(work);
    return rc;
}

int apsp_resign(uint8_t *sfo, size_t sfo_len)
{
    sfo_val_t params;
    int rc = sfo_params(sfo, sfo_len, &params);

    if (!sfo)
        return APSP_ERR_ARG;
    if (rc != APSP_OK)
        return rc;

    kirk_init_fuse(g_fuse_id);

    LOG("Resigning PARAM.SFO, %zu bytes, savedata mode %02X", sfo_len, sfo[params.off]);
    UpdateSavedataHashes(sfo + params.off, sfo, sfo_len);
    return APSP_OK;
}

/*
 * Vendored into apollo-patcher from bucanero/apollo-psp @ 17cb5ea
 * ("Use Mbedtls (#52)"), source/kirk_engine.c and include/kirk_engine.h.
 *
 * Two changes from upstream, both about running off a PSP:
 *
 *   - <psptypes.h> is gone; the u8/u16/u32/u64 typedefs come from <stdint.h>.
 *   - kirk_init() no longer reads the console's Fuse ID (a kernel-exploit read
 *     at 0xBC100090, falling back to ./DATA/FUSEID.BIN). A desktop or a browser
 *     tab has no fuse, so the value is a parameter here -- see kirk_init_fuse()
 *     and the note above it.
 *
 * Nothing else is touched, deliberately: the algorithms are proven byte-exact
 * against real console saves and this file should stay diffable against
 * upstream.
 *
 * The eventual home for this is apollo-lib/source/psp/, shared with apollo-psp
 * and apollo-vita instead of copied. That move waits on apollo-vita finishing
 * its own migration to mbedTLS -- its copy is still on polarSSL, which is the
 * only substantive difference between the two upstream versions. When it
 * lands, the backend split is the same #ifdef _USE_MBEDTLS / compat-1.3.h
 * shim that apollo-lib's source/decrypt.c already uses.
 */
/*
	Draan proudly presents:
	
	With huge help from community:
	coyotebean, Davee, hitchhikr, kgsws, liquidzigong, Mathieulh, Proxima, SilverSpring
	
	******************** KIRK-ENGINE ********************
	An Open-Source implementation of KIRK (PSP crypto engine) algorithms and keys.
	Includes also additional routines for hash forging.
	
	********************
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef KIRK_ENGINE
#define KIRK_ENGINE
#include <stdint.h>

/* Upstream takes these from the PSP SDK's <psptypes.h>. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

//Kirk return values
#define KIRK_OPERATION_SUCCESS 0
#define KIRK_NOT_ENABLED 1
#define KIRK_INVALID_MODE 2
#define KIRK_HEADER_HASH_INVALID 3
#define KIRK_DATA_HASH_INVALID 4
#define KIRK_SIG_CHECK_INVALID 5
#define KIRK_UNK_1 6
#define KIRK_UNK_2 7
#define KIRK_UNK_3 8
#define KIRK_UNK_4 9
#define KIRK_UNK_5 0xA
#define KIRK_UNK_6 0xB
#define KIRK_NOT_INITIALIZED 0xC
#define KIRK_INVALID_OPERATION 0xD
#define KIRK_INVALID_SEED_CODE 0xE
#define KIRK_INVALID_SIZE 0xF
#define KIRK_DATA_SIZE_ZERO 0x10

typedef struct
{
	int mode;    //0
	int unk_4;   //4
	int unk_8;   //8
	int keyseed; //C
	int data_size;   //10
} KIRK_AES128CBC_HEADER; //0x14

typedef struct
{
	u8  AES_key[16];            //0
	u8  CMAC_key[16];           //10
	u8  CMAC_header_hash[16];   //20
	u8  CMAC_data_hash[16];     //30
	u8  unused[32];             //40
	u32 mode;                   //60
	u8  ecdsa_hash;             //64
	u8  unk3[11];               //65
	u32 data_size;              //70
	u32 data_offset;            //74  
	u8  unk4[8];                //78
	u8  unk5[16];               //80
} KIRK_CMD1_HEADER; //0x90

typedef struct
{
	u8  AES_key[16];            //0
	u8  header_sig_r[20];           //10
	u8  header_sig_s[20];   //24
	u8  data_sig_r[20];     //38
	u8  data_sig_s[20];     //4C
	u32 mode;                   //60
	u8  ecdsa_hash;             //64
	u8  unk3[11];               //65
	u32 data_size;              //70
	u32 data_offset;            //74  
	u8  unk4[8];                //78
	u8  unk5[16];               //80
} KIRK_CMD1_ECDSA_HEADER; //0x90

typedef struct
{
	u8 r[0x14];
	u8 s[0x14];
} ECDSA_SIG; //0x28
typedef struct
{
	u8 x[0x14];
	u8 y[0x14];
} ECDSA_POINT; //0x28

typedef struct
{
    u32 data_size;             //0     
} KIRK_SHA1_HEADER;            //4

typedef struct
{
	u8 private_key[0x14];
	ECDSA_POINT public_key;
} KIRK_CMD12_BUFFER;

typedef struct
{
	u8 multiplier[0x14];
	ECDSA_POINT public_key;
} KIRK_CMD13_BUFFER;

typedef struct
{
	u8 enc_private[0x20];					//0
	u8 message_hash[0x14];				//20
} KIRK_CMD16_BUFFER;//0x34

typedef struct
{
	ECDSA_POINT public_key;					//0
	u8 message_hash[0x14];				//28
	ECDSA_SIG signature;					//3C
} KIRK_CMD17_BUFFER;//0x64

//mode passed to sceUtilsBufferCopyWithRange
#define KIRK_CMD_ENCRYPT_PRIVATE 0
#define KIRK_CMD_DECRYPT_PRIVATE 1
#define KIRK_CMD_2 2
#define KIRK_CMD_3 3
#define KIRK_CMD_ENCRYPT_IV_0 4
#define KIRK_CMD_ENCRYPT_IV_FUSE 5
#define KIRK_CMD_ENCRYPT_IV_USER 6
#define KIRK_CMD_DECRYPT_IV_0 7
#define KIRK_CMD_DECRYPT_IV_FUSE 8
#define KIRK_CMD_DECRYPT_IV_USER 9
#define KIRK_CMD_PRIV_SIGN_CHECK 10
#define KIRK_CMD_SHA1_HASH 11
#define KIRK_CMD_ECDSA_GEN_KEYS 12
#define KIRK_CMD_ECDSA_MULTIPLY_POINT 13
#define KIRK_CMD_PRNG 14
#define KIRK_CMD_15 15
#define KIRK_CMD_ECDSA_SIGN 16
#define KIRK_CMD_ECDSA_VERIFY 17

//"mode" in header
#define KIRK_MODE_CMD1 1
#define KIRK_MODE_CMD2 2
#define KIRK_MODE_CMD3 3
#define KIRK_MODE_ENCRYPT_CBC 4
#define KIRK_MODE_DECRYPT_CBC 5

//sceUtilsBufferCopyWithRange errors
#define SUBCWR_NOT_16_ALGINED 0x90A
#define SUBCWR_HEADER_HASH_INVALID 0x920
#define SUBCWR_BUFFER_TOO_SMALL 0x1000

/*
      // Private Sig + Cipher
      0x01: Super-Duper decryption (no inverse)
      0x02: Encrypt Operation (inverse of 0x03)
      0x03: Decrypt Operation (inverse of 0x02)

      // Cipher
      0x04: Encrypt Operation (inverse of 0x07) (IV=0)
      0x05: Encrypt Operation (inverse of 0x08) (IV=FuseID)
      0x06: Encrypt Operation (inverse of 0x09) (IV=UserDefined)
      0x07: Decrypt Operation (inverse of 0x04)
      0x08: Decrypt Operation (inverse of 0x05)
      0x09: Decrypt Operation (inverse of 0x06)
	  
      // Sig Gens
      0x0A: Private Signature Check (checks for private SCE sig)
      0x0B: SHA1 Hash
      0x0C: Mul1
      0x0D: Mul2
      0x0E: Random Number Gen
      0x0F: (absolutely no idea ? could be KIRK initialization)
      0x10: Signature Gen
      // Sig Checks
      0x11: Signature Check (checks for generated sigs)
      0x12: Certificate Check (idstorage signatures)
*/

//kirk-like funcs
int kirk_CMD0(u8* outbuff, u8* inbuff, int size, int generate_trash);
int kirk_CMD1(u8* outbuff, u8* inbuff, int size);
int kirk_CMD5(u8* outbuff, u8* inbuff, int size);
int kirk_CMD4(u8* outbuff, u8* inbuff, int size);
int kirk_CMD7(u8* outbuff, u8* inbuff, int size);
int kirk_CMD10(u8* inbuff, int insize);
int kirk_CMD11(u8* outbuff, u8* inbuff, int size);
int kirk_CMD14(u8* outbuff, int outsize);

/*
 * Bring the engine up. kirk_init() uses KIRK_HOST_FUSE_ID below.
 *
 * The Fuse ID is the console's own per-unit id. It reaches exactly one place:
 * kirk_CMD5 (KIRK_CMD_ENCRYPT_IV_FUSE), through the key mesh. Savedata
 * DECRYPTION never goes there -- it is CMD4/CMD7 against the fixed keyvault --
 * so a wrong fuse cannot affect reading a save. It matters only when writing
 * the PARAM.SFO hashes for savedata modes 4 and 6, and the PSP accepts those
 * even when they were generated elsewhere (see psp_savedata.c).
 *
 * Re-initialises when the id differs from the one already in effect, so a
 * front-end offering a FUSEID override does not have to tear anything down.
 */
int kirk_init(void); //CMD 0xF?
int kirk_init_fuse(u64 fuse_id);

/*
 * What a host with no fuse uses. This is also what apollo-psp itself falls
 * back to when the kernel read fails and no DATA/FUSEID.BIN is present, so it
 * is an existing path rather than a new one.
 */
#define KIRK_HOST_FUSE_ID 0xFFFFFFFFFFFFFFFFull

//sce-like funcs
int sceUtilsBufferCopyWithRange(u8* outbuff, int outsize, u8* inbuff, int insize, int cmd);

#endif

/*
*
* Copyright (c) 2008 Andreas Weis (http://www.ghulbus-inc.de/)
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy  of  this   software  and  associated   documentation  files  (the
* "Software"),  to deal  in the Software  without  restriction, including
* without  limitation  the rights to  use, copy,  modify, merge, publish,
* distribute,  sublicense, and/or  sell  copies of the  Software, and  to
* permit persons to  whom the Software is furnished  to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR   IMPLIED,  INCLUDING   BUT  NOT  LIMITED  TO   THE  WARRANTIES   OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR  PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE  AUTHORS OR COPYRIGHT HOLDERS  BE LIABLE FOR  ANY
* CLAIM,  DAMAGES  OR OTHER  LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT  OR OTHERWISE,  ARISING FROM,  OUT OF  OR IN  CONNECTION  WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/*
 * PORTED from apollo-ps4 (include/ps2icon.h). The two entry points that read a
 * buffer are kept; ps2icon_load() and getIconPS2(), which pull an icon off a
 * mounted memory card, are not -- see core/ps2/ps2icon.c.
 *
 * ps2_IconSys_t is declared here rather than pulled in from a memory-card
 * header, because it is the one structure from that world this build needs:
 * the renderer takes its lighting and its background from it.
 */
#ifndef PS2ICON_H
#define PS2ICON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


//================================================================================================
//   Typedefs and Defines
//================================================================================================


/* icon.sys, as it sits inside a .PSV. Only the fields the renderer reads are
 * named; the rest is padding to keep the offsets right. */
typedef struct {
    char     magic[4];
    uint16_t padding1;
    uint16_t secondLineOffset;
    uint32_t padding2;
    uint32_t transparencyVal;
    uint8_t  bgColourUpperLeft[16];
    uint8_t  bgColourUpperRight[16];
    uint8_t  bgColourLowerLeft[16];
    uint8_t  bgColourLowerRight[16];
    uint8_t  light1Direction[16];
    uint8_t  light2Direction[16];
    uint8_t  light3Direction[16];
    uint8_t  light1RGB[16];
    uint8_t  light2RGB[16];
    uint8_t  light3RGB[16];
    uint8_t  ambientLightRGB[16];
    char     title[68];           /* NUL-terminated, Shift-JIS */
    char     IconName[64];
    char     copyIconName[64];
    char     deleteIconName[64];
    uint8_t  padding3[512];
} ps2_IconSys_t;


/** File header
 */
typedef struct Icon_Header_t {
	unsigned int file_id;						///< reserved; should be: 0x010000 (but does not have to ;) )
	unsigned int animation_shapes;				///< number of animation shapes per vertex
	unsigned int texture_type;					///< texture type - 0x07: uncompressed, 0x06: uncompresses, 0x0f: RLE compression
	unsigned int reserved;						///< reserved; should be: 0x3F800000 (but does not have to ;) )
	unsigned int n_vertices;					///< number of vertices; must be a multiple of 3
} Icon_Header;
/** Set of vertex coordinates
 * @note The f16_* fields indicate float16 data; divide by 4096.0f to convert to float32;
 */
typedef struct Vertex_Coord_t {
	short f16_x;								///< vertex x coordinate in float16
	short f16_y;								///< vertex y coordinate in float16
	short f16_z;								///< vertex z coordinate in float16
	short f16_unknown;							///< unknown; seems to influence lightning?
} Vertex_Coord;
/** Set of texture coordinates
 * @note The f16_* fields indicate float16 data; divide by 4096.0f to convert to float32;
 */
typedef struct Texture_Data_t {
	short        f16_u;							///< vertex u texture coordinate in float16
	short        f16_v;							///< vertex v texture coordinate in float16
	unsigned int color;							///< vertex color (32 bit RGBA)
} Texture_Data;
/** Animation header
 */
typedef struct Animation_Header_t {
	unsigned int id_tag;						///< ???
	unsigned int frame_length;					///< ???
	float        anim_speed;					///< ???
	unsigned int play_offset;					///< ???
	unsigned int n_frames;						///< number of frames in the animation
} Animation_Header;
/** Per-frame animation data
 */
typedef struct Frame_Data_t {
	unsigned int shape_id;						///< shape used for this frame
	unsigned int n_keys;						///< number of keys corresponding to this frame
} Frame_Data;
/** Per-key animation data
 */
typedef struct Frame_Key_t {
	float time;									///< ???
	float value;								///< ???
} Frame_Key;

/*
 * These structures are memcpy'd straight out of a .ico, so their layout IS the
 * file format. Asserted rather than assumed: every member above is naturally
 * aligned and no padding should ever appear, but an ABI that disagreed would
 * not fail loudly -- it would shift every field and produce an icon made of
 * noise, on whichever platform happened to differ.
 *
 * ps2_IconSys_t's 964 is the same number every real icon.sys in the save
 * database reports as its own size, which is a second opinion on the layout.
 *
 * Written to compile as both C and C++: the desktop app includes this header
 * from main.cpp.
 */
#ifdef __cplusplus
#define APOLLO_ICON_SASSERT(c, m) static_assert(c, m)
#else
#define APOLLO_ICON_SASSERT(c, m) _Static_assert(c, m)
#endif

APOLLO_ICON_SASSERT(sizeof(Icon_Header)      == 20,  "Icon_Header is padded");
APOLLO_ICON_SASSERT(sizeof(Vertex_Coord)     ==  8,  "Vertex_Coord is padded");
APOLLO_ICON_SASSERT(sizeof(Texture_Data)     ==  8,  "Texture_Data is padded");
APOLLO_ICON_SASSERT(sizeof(Animation_Header) == 20,  "Animation_Header is padded");
APOLLO_ICON_SASSERT(sizeof(Frame_Data)       ==  8,  "Frame_Data is padded");
APOLLO_ICON_SASSERT(sizeof(Frame_Key)        ==  8,  "Frame_Key is padded");
APOLLO_ICON_SASSERT(sizeof(ps2_IconSys_t)    == 964, "ps2_IconSys_t is padded");

/** A parsed icon: the morph targets, the per-vertex attributes they share,
 *  and the 128x128 texture.
 *
 *  `texture` is always allocated, even for an icon that carries none - those
 *  are drawn from their vertex colours alone, and a cleared buffer is what the
 *  existing texture export has always handed back. `has_texture` says which it
 *  is, so a renderer can substitute white instead of black.
 */
typedef struct {
	int       shape_count;      ///< morph targets, at least 1
	int       vertex_count;     ///< a multiple of 3
	float    *shapes;           ///< shape_count * vertex_count * 3
	float    *normals;          ///< vertex_count * 3
	float    *uvs;              ///< vertex_count * 2
	uint8_t  *colors;           ///< vertex_count * 4, RGBA, 0x80-centred
	uint32_t *texture;          ///< 128 * 128 RGBA, never NULL after a parse
	int       has_texture;
	int       still_shape;      ///< the shape a still frame should use

	/* The animation, for a caller that wants to play it rather than take a
	 * still. `order` is which shape each frame uses -- the file's own frame
	 * list, or simply every shape in turn when it carries none -- and the
	 * model morphs linearly from one to the next.
	 *
	 * Most icons do not animate at all: of 2,345 in the save database, 1,898
	 * carry a single shape and there is nothing to move between. `frame_count`
	 * is 1 for those. */
	int      *order;            ///< frame_count entries, each a shape index
	int       frame_count;
	uint32_t  frame_length;     ///< loop length in 60Hz display frames
	float     anim_speed;
} ps2icon_t;

/** Parse an .ico.
 *
 *   0   a usable model: geometry, and a texture when the file carries one
 *   1   NO usable geometry, but a whole texture was recovered. `vertex_count`
 *       is 0 and `texture` is a complete 128x128 image -- draw that flat.
 *  <0   nothing usable. The file is damaged, and a caller should say so
 *       rather than quietly showing nothing: an icon that will not parse means
 *       the savedata around it is suspect.
 *
 * `texture` is allocated in every case, so it is always safe to read; only a
 * return of 0 or 1 says anything was put in it. Free with ps2icon_free(). */
int ps2icon_parse(const uint8_t *data, size_t len, ps2icon_t *out);

void ps2icon_free(ps2icon_t *icon);

/** How long one loop of the animation lasts, in seconds. Clamped to something
 *  watchable (0.3s to 10s), and 0 for an icon that does not animate. */
float ps2icon_loop_seconds(const ps2icon_t *icon);

/** Which two shapes the icon is between at time `t` seconds into the loop, and
 *  how far (0..1). Both come back equal, with `morph` 0, for a still icon. */
void ps2icon_morph_at(const ps2icon_t *icon, float t,
                      int *shape_a, int *shape_b, float *morph);

/**
 * How far the model has turned at `t` seconds, in radians.
 *
 * These icons were shown on a turntable, and the rotation is not decoration:
 * it is what shows a model to BE one. Gran Turismo's is a cube with the logo
 * on its faces, and held still it is a flat square.
 *
 * It applies to EVERY icon, not only the ones that morph -- 1,898 of the 2,345
 * in the save database carry a single shape and have nothing to morph, and
 * those are exactly the ones a still frame misrepresents.
 *
 * The rate is icon3d.js's `state.yaw = t * 0.5`: one turn every 4*pi seconds,
 * about 12.6. `t` is the same clock that drives ps2icon_morph_at().
 */
#define PS2ICON_SPIN_RATE  0.5f   /* radians a second; icon3d.js's `t * 0.5` */

float ps2icon_yaw_at(float t);


#ifdef __cplusplus
}
#endif

#endif

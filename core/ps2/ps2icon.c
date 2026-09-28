/*
 * ps2icon - the 3D icon a PS2 save carries.
 *
 * A PS2 memory-card save keeps its own dashboard presentation inside it: an
 * icon.sys naming the save and its lighting, and one to three .ico files, each
 * a small textured 3D model the console spun on its save list. There is no
 * flat image anywhere -- so unlike every other console here, showing a PS2
 * save's icon means rendering one.
 *
 * This is the parser. core/ps2/ps2render.c is the rasteriser, and together
 * they turn a .ico plus its icon.sys into the RGBA a texture wants.
 *
 * PORTED from apollo-ps4 (source/ps2icon.c), which credits the format notes of
 * Andreas Weis. Trimmed to the two entry points that take a buffer: the
 * original also reads icons off a mounted memory card through mcio, and
 * nothing here has a memory card to mount -- a save arrives as a .PSV, and
 * core/psvcard.c has already lifted the file out by the time this is called.
 *
 * Bounds. Every count in a .ico is read out of the file, and the file came out
 * of somebody's save, so each is checked against the real length before it is
 * followed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <math.h>

#include "ps2icon.h"
#include "ps2render.h"

/* apollo-ps4 has this in its util.h, which is a PS4 header. It is two lines. */
static uint16_t read_le_uint16(const uint8_t *buf)
{
    return (uint16_t)((uint32_t)buf[0] | ((uint32_t)buf[1] << 8));
}


static uint32_t TIM2RGBA(const uint8_t *buf)
{
	uint8_t RGBA[4];
	uint16_t lRGB = read_le_uint16(buf);

	RGBA[0] = 8 * (lRGB & 0x1F);
	RGBA[1] = 8 * ((lRGB >> 5) & 0x1F);
	RGBA[2] = 8 * (lRGB >> 10);
	RGBA[3] = 0xFF;

	return *((uint32_t *) &RGBA);
}

//Bytes still readable at 'off' in a buffer of 'len' bytes
#define ICON_AVAIL(len, off)	(((off) < (len)) ? ((len) - (off)) : 0)

//Texels the output buffer can still take
#define ICON_TEXELS		(128 * 128)

void ps2icon_free(ps2icon_t *icon)
{
	if (!icon)
		return;

	free(icon->order);
	free(icon->shapes);
	free(icon->normals);
	free(icon->uvs);
	free(icon->colors);
	free(icon->texture);
	memset(icon, 0, sizeof(*icon));
}

/*
 * Geometry is unusable. Is there at least a whole picture in here?
 *
 * An icon whose vertex table does not fit its file cannot be drawn as a model,
 * but its TEXTURE is a complete 128x128 image in its own right, and showing
 * that is better than showing nothing -- it is what the model was wearing.
 *
 * Only the UNCOMPRESSED form is recovered, and only when the file is long
 * enough to hold one whole texture: then it is exactly ICON_TEXELS*2 bytes at
 * a known distance from the end, so finding it is arithmetic rather than a
 * guess.
 *
 * An RLE texture is deliberately NOT hunted for. Its position depends on where
 * the geometry ended, which is the very thing that is wrong, and a search for
 * "a stream that fills 128x128 and stops at the end of the file" is not
 * discriminating: run against the Action Replay MAX icon it finds two such
 * offsets, and both decode to noise. A convincing wrong picture is worse than
 * none, because the caller then has no reason to tell anyone the save is
 * damaged.
 *
 * Returns 1 when a texture was recovered, or `ok` (negative) when not.
 */
static int recover_texture(const uint8_t *iData, size_t len, ps2icon_t *out)
{
	Icon_Header header;
	size_t at;
	int i;

	if (len < sizeof(Icon_Header))
		return -1;

	memcpy(&header, iData, sizeof(Icon_Header));
	if (header.texture_type > 7)
		return -1;                        /* RLE: see above */

	if (len < sizeof(Icon_Header) + (size_t)ICON_TEXELS * 2)
		return -1;                        /* no room for a whole one */

	at = len - (size_t)ICON_TEXELS * 2;
	for (i = 0; i < ICON_TEXELS; i++, at += 2)
		out->texture[i] = TIM2RGBA(&iData[at]);

	out->has_texture = 1;
	out->vertex_count = 0;                /* nothing to draw it on */
	return 1;
}

/* 12.4 fixed point, as the file stores every coordinate. */
#define ICON_F16(v)  ((float)(int16_t)(v) / 4096.0f)

int ps2icon_parse(const uint8_t* iData, size_t len, ps2icon_t *out)
{
	uint32_t i;
	uint16_t j;
	Icon_Header header;
	Animation_Header anim_header;
	Frame_Data animation;
	uint32_t *lTexturePtr, *lRGBA;
	size_t offset = 0, vertex_size, geom = 0;
	int s_i, v_i, ok = -1;

	if (!out)
		return -1;

	memset(out, 0, sizeof(*out));

	lTexturePtr = (uint32_t *) calloc(ICON_TEXELS, sizeof(uint32_t));
	if (!lTexturePtr)
		return -1;

	out->texture = lTexturePtr;

	//read header:
	if (len < sizeof(Icon_Header))
		return ok;

	memcpy(&header, iData, sizeof(Icon_Header));
	offset += sizeof(Icon_Header);

	//n_vertices has to be divisible by three, that's for sure:
	if(header.file_id != 0x010000 || header.n_vertices % 3 != 0)
		return ok;

	//a vertex needs at least one animation shape, and the count is bounded so
	//the size calculation below cannot overflow
	if(header.animation_shapes == 0 || header.animation_shapes > 0xFFFF)
		return ok;

	//read icon data from file: https://ghulbus-inc.de/projects/ps2iconsys/
	///Vertex data
	// each vertex consists of animation_shapes tuples for vertex coordinates,
	// followed by one vertex coordinate tuple for normal coordinates
	// followed by one texture data tuple for texture coordinates and color
	vertex_size = (size_t)sizeof(Vertex_Coord) * header.animation_shapes
			+ sizeof(Vertex_Coord) + sizeof(Texture_Data);

	/*
	 * The count is read out of the file and can exceed it.
	 *
	 * Refused outright, and deliberately NOT clamped to what fits. Drawing
	 * the part that is present produces a plausible-looking picture of a
	 * damaged save -- the Action Replay MAX icon in the database declares
	 * 1,770 vertices in a file with room for 1,159, and the two thirds that
	 * survive render as a clean silhouette that says nothing is wrong. A
	 * caller is better told the file is broken: see ps2icon_parse's contract,
	 * and the texture recovery below, which is the one fallback worth having
	 * because a whole texture is a whole picture.
	 *
	 * Divided rather than multiplied out, so the arithmetic cannot overflow.
	 */
	if (header.n_vertices > ICON_AVAIL(len, offset) / vertex_size)
		return recover_texture(iData, len, out);

	//pull the geometry out on the way past. A renderer needs every shape
	//(they are morph targets), the normals, and the texture coordinates and
	//colour each vertex carries.
	out->shape_count = (int)header.animation_shapes;
	out->vertex_count = (int)header.n_vertices;
	out->shapes = malloc(sizeof(float) * 3 * header.animation_shapes * header.n_vertices);
	out->normals = malloc(sizeof(float) * 3 * header.n_vertices);
	out->uvs = malloc(sizeof(float) * 2 * header.n_vertices);
	out->colors = malloc(4 * (size_t)header.n_vertices);

	if (!out->shapes || !out->normals || !out->uvs || !out->colors) {
		ps2icon_free(out);
		return -1;
	}

	geom = offset;
	for (v_i = 0; v_i < out->vertex_count; v_i++) {
		for (s_i = 0; s_i < out->shape_count; s_i++) {
			const uint8_t *p = &iData[geom];
			float *dst = &out->shapes[((size_t)s_i * out->vertex_count + v_i) * 3];

			dst[0] = ICON_F16(read_le_uint16(&p[0]));
			dst[1] = ICON_F16(read_le_uint16(&p[2]));
			dst[2] = ICON_F16(read_le_uint16(&p[4]));
			geom += sizeof(Vertex_Coord);
		}

		out->normals[v_i * 3 + 0] = ICON_F16(read_le_uint16(&iData[geom + 0]));
		out->normals[v_i * 3 + 1] = ICON_F16(read_le_uint16(&iData[geom + 2]));
		out->normals[v_i * 3 + 2] = ICON_F16(read_le_uint16(&iData[geom + 4]));
		geom += sizeof(Vertex_Coord);

		out->uvs[v_i * 2 + 0] = ICON_F16(read_le_uint16(&iData[geom + 0]));
		out->uvs[v_i * 2 + 1] = ICON_F16(read_le_uint16(&iData[geom + 2]));
		memcpy(&out->colors[v_i * 4], &iData[geom + 4], 4);
		geom += sizeof(Texture_Data);
	}

	offset += vertex_size * header.n_vertices;

	//animation data
	// preceeded by an animation header, there is a frame data/key set for every frame:
	if (ICON_AVAIL(len, offset) < sizeof(Animation_Header))
		return ok;

	memcpy(&anim_header, &iData[offset], sizeof(Animation_Header));
	offset += sizeof(Animation_Header);

	/* The loop's own timing, which a caller that plays the animation needs.
	 * Kept verbatim; ps2icon_loop_seconds() turns it into a duration. */
	out->frame_length = anim_header.frame_length;
	out->anim_speed   = anim_header.anim_speed;

	/* Which shape each frame uses. The count comes out of the file, so it is
	 * checked against the real length before anything is allocated for it. */
	if (anim_header.n_frames > 0 &&
	    anim_header.n_frames <= ICON_AVAIL(len, offset) / sizeof(Frame_Data)) {
		out->order = malloc(sizeof(int) * anim_header.n_frames);
		if (!out->order) {
			ps2icon_free(out);
			return -1;
		}
	}

	//read animation data:
	for(i=0; i<anim_header.n_frames; i++) {
		if (ICON_AVAIL(len, offset) < sizeof(Frame_Data))
			return ok;

		memcpy(&animation, &iData[offset], sizeof(Frame_Data));
		offset += sizeof(Frame_Data);

		if (out->order) {
			int id = (int)animation.shape_id;
			out->order[out->frame_count++] =
				id < out->shape_count ? id : out->shape_count - 1;
		}

		/* The still the web thumbnailer draws is the first frame's shape, but
		 * only when there is a sequence to interpolate: with fewer than two
		 * frames it falls back to shape 0. Matching that keeps the two
		 * renderers showing the same pose. */
		if (i == 0 && anim_header.n_frames >= 2)
			out->still_shape = (int)animation.shape_id < out->shape_count
					 ? (int)animation.shape_id : out->shape_count - 1;

		if (animation.n_keys > ICON_AVAIL(len, offset) / sizeof(Frame_Key))
			return ok;

		offset += sizeof(Frame_Key) * animation.n_keys;
	}

	/*
	 * An icon with shapes but no frame list animates through them in order --
	 * the same fallback the reference web renderer uses, so the two agree
	 * about what a given save looks like moving.
	 */
	if (out->frame_count < 2 && out->shape_count > 1) {
		free(out->order);
		out->order = malloc(sizeof(int) * out->shape_count);
		out->frame_count = 0;
		if (out->order)
			while (out->frame_count < out->shape_count) {
				out->order[out->frame_count] = out->frame_count;
				out->frame_count++;
			}
	}

	//everything the renderer needs has been read; the texture is a bonus
	ok = 0;

	lRGBA = lTexturePtr;

	if (header.texture_type <= 7)
	{	// Uncompressed texture
		// Some icons carry no texture at all: the file ends after the animation
		// block and the model is drawn from its vertex colours. Hand back the
		// cleared buffer rather than reading past the end of the file.
		if (ICON_AVAIL(len, offset) < (ICON_TEXELS * 2))
			return ok;

		for (i = 0; i < ICON_TEXELS; i++, offset += 2)
			*lRGBA++ = TIM2RGBA(&iData[offset]);
	}
	else
	{	//Compressed texture
		offset += 4;

		while ((lRGBA - lTexturePtr) < ICON_TEXELS)
		{
			if (ICON_AVAIL(len, offset) < 2)
				break;

			j = read_le_uint16(&iData[offset]);

			if (j & 0x8000)
			{	//a run of literal texels: 0x10000 - j of them, so up to 32768
				for (j = (0x0000 - j) & 0xFFFF; j > 0; j--)
				{
					offset += 2;

					if (ICON_AVAIL(len, offset) < 2 || (lRGBA - lTexturePtr) >= ICON_TEXELS)
						break;

					*lRGBA++ = TIM2RGBA(&iData[offset]);
				}
			}
			else
			{	//one texel repeated j times
				offset += 2;

				if (ICON_AVAIL(len, offset) < 2)
					break;

				for (; j > 0; j--)
				{
					if ((lRGBA - lTexturePtr) >= ICON_TEXELS)
						break;

					*lRGBA++ = TIM2RGBA(&iData[offset]);
				}
			}
			offset += 2;
		}
	}

	/* Only a full 128x128 counts. A truncated RLE stream leaves the tail of
	 * the buffer zeroed, and handing that to the renderer as a texture draws
	 * the model black; the JavaScript parser reports the same case as
	 * textureless so the model falls back to its vertex colours. */
	out->has_texture = (lRGBA - lTexturePtr) == ICON_TEXELS;

	return ok;
}

/*
 * How long one loop lasts.
 *
 * The file states it in 60Hz display frames with a speed multiplier over the
 * top, and both come out of the file, so the result is clamped rather than
 * trusted: a bad value would otherwise freeze the icon for minutes or flicker
 * it past seeing.
 */
float ps2icon_loop_seconds(const ps2icon_t *icon)
{
	float secs;

	if (!icon || icon->frame_count < 2)
		return 0.0f;

	if (!icon->frame_length)
		return (float)icon->frame_count / 8.0f;   /* no timing: 8 shapes a second */

	secs = icon->anim_speed > 0.0f
	     ? (float)icon->frame_length / (60.0f * icon->anim_speed)
	     : (float)icon->frame_length / 60.0f;

	/* Written as a negated comparison so a NaN lands on the floor too. */
	if (!(secs > 0.3f)) secs = 0.3f;
	if (secs > 10.0f)   secs = 10.0f;
	return secs;
}

void ps2icon_morph_at(const ps2icon_t *icon, float t,
                      int *shape_a, int *shape_b, float *morph)
{
	float loop, pos;
	int n, i;

	if (shape_a) *shape_a = icon ? icon->still_shape : 0;
	if (shape_b) *shape_b = icon ? icon->still_shape : 0;
	if (morph)   *morph   = 0.0f;

	if (!icon || icon->frame_count < 2 || !icon->order)
		return;

	loop = ps2icon_loop_seconds(icon);
	if (loop <= 0.0f)
		return;

	n = icon->frame_count;

	/* Wrapped with fmodf rather than by subtracting in a loop: `t` is a clock
	 * that keeps counting, and a window left open all afternoon would spin. */
	pos = fmodf(t, loop);
	if (pos < 0.0f) pos += loop;
	pos = pos / loop * (float)n;

	i = (int)pos;
	if (i < 0)  i = 0;
	if (i >= n) i = n - 1;            /* only reachable through rounding */

	if (shape_a) *shape_a = icon->order[i];
	if (shape_b) *shape_b = icon->order[(i + 1) % n];
	if (morph)   *morph   = pos - (float)i;
}

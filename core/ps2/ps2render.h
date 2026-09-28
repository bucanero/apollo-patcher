/*
 * ps2render.h - software renderer for PS2 save icons
 *
 * A PS2 save's icon is a textured 3D model, not a picture, so showing one
 * means drawing it. This rasterises the model in software -- no GL context,
 * no GPU -- and hands back RGBA, which is the same floor the rest of this app
 * targets and the only thing that works in the wasm build.
 *
 * PORTED from apollo-ps4 (source/ps2render.c) unchanged. It mirrors that
 * project's WebGL renderer deliberately rather than improving on it: the two
 * are compared against each other, so any difference is a bug in one of them,
 * and a third variant here would make that comparison meaningless.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef PS2RENDER_H
#define PS2RENDER_H

#include <stdint.h>
#include <stddef.h>

#include "ps2icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What goes behind the model. */
enum ps2render_bg {
	PS2RENDER_BG_TRANSPARENT = 0,   /* alpha 0, so the PNG composites anywhere */
	PS2RENDER_BG_GRADIENT           /* the four icon.sys corner colours */
};

/*
 * Render a still frame of `icon` into a `size` x `size` RGBA buffer.
 *
 * `sys` supplies the three directional lights, the ambient term and (for
 * PS2RENDER_BG_GRADIENT) the background corners; pass NULL to use the same
 * fallback lighting the web renderer uses for a save with no readable
 * icon.sys. `supersample` is the factor each axis is rendered at before being
 * averaged down - 1 disables it, 4 is plenty.
 *
 * `*out` is malloc'd and owned by the caller. Returns 0, or negative on bad
 * arguments or an allocation failure.
 */
int ps2icon_render(const ps2icon_t *icon, const ps2_IconSys_t *sys,
		   int size, int supersample, enum ps2render_bg bg,
		   uint8_t **out);

/*
 * The same, at one point in the animation: the model is drawn morphed `morph`
 * of the way from shape_a to shape_b, which is what ps2icon_morph_at() works
 * out for a given time. Positions are interpolated and nothing else, matching
 * the reference renderer's vertex shader.
 *
 * The model is sized over EVERY shape rather than the pose being drawn, so an
 * animation cannot swim in and out of frame as it plays.
 */
int ps2icon_render_at(const ps2icon_t *icon, const ps2_IconSys_t *sys,
		      int size, int supersample, enum ps2render_bg bg,
		      int shape_a, int shape_b, float morph, uint8_t **out);

#ifdef __cplusplus
}
#endif

#endif

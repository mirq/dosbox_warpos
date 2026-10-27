/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Accelerated Voodoo output through MiniGL V19 (WarpOS).
 *
 * Implements the voodoo_ogl_* hooks the SST-1 core (voodoo_emu.cpp) calls in
 * accelerated mode, with the same contract as DOSBox-X's voodoo_opengl.cpp.
 * The vertex setup (ogl_get_vertex_data and its depth/fog helpers) and the
 * texture cache are taken from that file; the GLSL combiner it builds per
 * raster state has no MiniGL equivalent, so the SST-1 colour/alpha/texture
 * combine units are mapped onto fixed-function GL here:
 *  - combine terms that do not involve a texel are evaluated per vertex and
 *    sent as the vertex colour;
 *  - texel terms become GL_MODULATE (texel * colour) or GL_DECAL (lerp of
 *    colour and texel by texel alpha), with two texture units when TMU0
 *    modulates TMU1;
 *  - chroma keying on texels is folded into the converted texture's alpha
 *    and an alpha test;
 *  - fog is applied to the vertex colour, which is exact for black fog.
 * Perspective comes from homogeneous vertices (glVertex4f with w = 1/oow):
 * MiniGL's glTexCoord4f divides by q immediately.
 *
 * Without C_VOODOO_MINIGL the hooks are stubs and the core stays in software
 * mode, like DOSBox-X's build without C_OPENGL. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>

#include "dosbox.h"

#if C_VOODOO_MINIGL
/* After <stdlib.h>: minigl_dispatch.h redeclares atexit() without C
 * linkage. Before the voodoo headers: voodoo_def.h #defines register names
 * such as color0 and fogColor. */
#include <proto/minigl.h>
#endif

#include "control.h"
#include "setup.h"
#include "logging.h"

#include "voodoo_emu.h"
#include "voodoo_opengl.h"

bool Voodoo_OGL_Active() {
	return (v != NULL && v->clock_enabled && v->output_on);
}

#if C_VOODOO_MINIGL

#include "keyboard.h"
#include "mouse.h"
#include "voodoo_def.h"
#include "wos_mglinput.h"
#include "wos_video.h"

/* size of the MiniGL context; the core's coordinates are scaled to it */
UINT32 new_width = 0;
UINT32 new_height = 0;

static bool mgl_lib_open = false;	/* MiniGLOpen() succeeded */
static bool mgl_lib_failed = false;	/* don't retry a missing DLL/host every activation */
static bool mgl_ctx = false;		/* a context (screen or window) is open */
static bool mgl_fullscreen = true;
static int mgl_depth = 16;

static INT32 last_orientation = -1;
static UINT32 last_width = 0, last_height = 0;
static UINT32 last_clear_color = 0;
static bool front_dirty = false;	/* drew to the front buffer since the last present */

/* LFB read line caches (glReadPixels is a synchronous GPU drain) */
static INT32 cached_line_y[2] = { -1, -1 };
static UINT8 *cached_line[2] = { NULL, NULL };
static UINT32 cached_line_len = 0;

/*************************************
 *  GL state cache
 *
 *  MiniGL rejects state changes inside glBegin/glEnd, and every glBegin
 *  costs a batch, so consecutive primitives of one kind share a glBegin.
 *  All state setters close an open primitive first.
 *************************************/

static GLenum begin_mode = 0;
static bool in_begin = false;

static void mgl_end(void) {
	if (in_begin) {
		glEnd();
		in_begin = false;
	}
}

static void mgl_begin(GLenum mode) {
	if (in_begin && begin_mode == mode) return;
	mgl_end();
	glBegin(mode);
	begin_mode = mode;
	in_begin = true;
}

static struct {
	int depth_test;			/* -1 unknown */
	GLenum depth_func;
	int depth_mask;
	int color_mask;
	int blend;
	GLenum blend_src, blend_dst;
	int alpha_test;
	GLenum alpha_func;
	GLfloat alpha_ref;
	int draw_front;
	int tex_enabled[2];
	GLuint tex_bound[2];
	GLenum tex_env[2];
	int active_unit;
} gs;

static void mgl_reset_state_cache(void) {
	memset(&gs, 0, sizeof(gs));
	gs.depth_test = gs.depth_mask = gs.color_mask = gs.blend = gs.alpha_test = gs.draw_front = -1;
	gs.tex_enabled[0] = gs.tex_enabled[1] = -1;
	gs.tex_env[0] = gs.tex_env[1] = 0;
	gs.tex_bound[0] = gs.tex_bound[1] = 0;
	gs.active_unit = -1;
	in_begin = false;
}

static void mgl_set_depth(bool enable, GLenum func) {
	if (gs.depth_test != (int)enable) {
		mgl_end();
		if (enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		gs.depth_test = enable;
	}
	if (enable && gs.depth_func != func) {
		mgl_end();
		glDepthFunc(func);
		gs.depth_func = func;
	}
}

static void mgl_set_depth_mask(bool mask) {
	if (gs.depth_mask != (int)mask) {
		mgl_end();
		glDepthMask(mask ? GL_TRUE : GL_FALSE);
		gs.depth_mask = mask;
	}
}

/* the MiniGL screen is 16/32-bit without destination alpha, only RGB matters */
static void mgl_set_color_mask(bool rgb) {
	if (gs.color_mask != (int)rgb) {
		mgl_end();
		const GLboolean m = rgb ? GL_TRUE : GL_FALSE;
		glColorMask(m, m, m, GL_FALSE);
		gs.color_mask = rgb;
	}
}

static void mgl_set_blend(bool enable, GLenum src, GLenum dst) {
	if (gs.blend != (int)enable) {
		mgl_end();
		if (enable) glEnable(GL_BLEND); else glDisable(GL_BLEND);
		gs.blend = enable;
	}
	if (enable && (gs.blend_src != src || gs.blend_dst != dst)) {
		mgl_end();
		glBlendFunc(src, dst);
		gs.blend_src = src;
		gs.blend_dst = dst;
	}
}

static void mgl_set_alpha_test(bool enable, GLenum func, GLfloat ref) {
	if (gs.alpha_test != (int)enable) {
		mgl_end();
		if (enable) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
		gs.alpha_test = enable;
	}
	if (enable && (gs.alpha_func != func || gs.alpha_ref != ref)) {
		mgl_end();
		glAlphaFunc(func, ref);
		gs.alpha_func = func;
		gs.alpha_ref = ref;
	}
}

static void mgl_set_draw_front(bool front) {
	if (front) {
		front_dirty = true;
		v->fbi.vblank_flush_pending = true;
		cached_line_y[0] = -1;
	} else {
		cached_line_y[1] = -1;
	}
	/* a windowed MiniGL context has a single surface; the vblank flush
	 * presents front-buffer drawing there */
	if (!mgl_fullscreen) return;
	if (gs.draw_front != (int)front) {
		mgl_end();
		glDrawBuffer(front ? GL_FRONT : GL_BACK);
		gs.draw_front = front;
	}
}

static void mgl_select_unit(int unit) {
	if (gs.active_unit != unit) {
		mgl_end();
		glActiveTextureARB(unit ? GL_TEXTURE1_ARB : GL_TEXTURE0_ARB);
		gs.active_unit = unit;
	}
}

static void mgl_set_texture(int unit, GLuint tex, GLenum env) {
	if (!tex) {
		if (gs.tex_enabled[unit] != 0) {
			mgl_select_unit(unit);
			glDisable(GL_TEXTURE_2D);
			gs.tex_enabled[unit] = 0;
		}
		return;
	}
	if (gs.tex_enabled[unit] != 1) {
		mgl_select_unit(unit);
		glEnable(GL_TEXTURE_2D);
		gs.tex_enabled[unit] = 1;
	}
	if (gs.tex_bound[unit] != tex) {
		mgl_select_unit(unit);
		glBindTexture(GL_TEXTURE_2D, tex);
		gs.tex_bound[unit] = tex;
	}
	if (gs.tex_env[unit] != env) {
		mgl_select_unit(unit);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
		gs.tex_env[unit] = env;
	}
}

static void mgl_untextured(void) {
	mgl_set_texture(1, 0, 0);
	mgl_set_texture(0, 0, 0);
}

/* Voodoo depth function (0..7) maps 1:1 onto GL_NEVER..GL_ALWAYS */
static void mgl_apply_depth_state(UINT32 FBZMODE) {
	if (FBZMODE_ENABLE_ALPHA_PLANES(FBZMODE) == 0) {
		if (FBZMODE_ENABLE_DEPTHBUF(FBZMODE)) {
			mgl_set_depth(true, GL_NEVER + FBZMODE_DEPTH_FUNCTION(FBZMODE));
		} else if (FBZMODE_AUX_BUFFER_MASK(FBZMODE) > 0) {
			/* GL only writes depth with the test enabled */
			mgl_set_depth(true, GL_ALWAYS);
		} else {
			mgl_set_depth(false, GL_ALWAYS);
		}
	} else {
		mgl_set_depth(true, GL_ALWAYS);
	}
	mgl_set_depth_mask(FBZMODE_AUX_BUFFER_MASK(FBZMODE) > 0 && FBZMODE_ENABLE_ALPHA_PLANES(FBZMODE) == 0);
	mgl_set_color_mask(FBZMODE_RGB_BUFFER_MASK(FBZMODE) > 0);
}

static const GLenum mgl_sfactor[16] = {
	GL_ZERO, GL_SRC_ALPHA, GL_DST_COLOR, GL_DST_ALPHA,
	GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_DST_ALPHA,
	GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO,
	GL_SRC_ALPHA_SATURATE
};

static const GLenum mgl_dfactor[16] = {
	GL_ZERO, GL_SRC_ALPHA, GL_SRC_COLOR, GL_DST_ALPHA,
	GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_ALPHA,
	GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO,
	GL_SRC_COLOR /* A_COLORBEFOREFOG */
};

static void mgl_apply_blend_state(UINT32 ALPHAMODE) {
	if (ALPHAMODE_ALPHABLEND(ALPHAMODE))
		mgl_set_blend(true, mgl_sfactor[ALPHAMODE_SRCRGBBLEND(ALPHAMODE)], mgl_dfactor[ALPHAMODE_DSTRGBBLEND(ALPHAMODE)]);
	else
		mgl_set_blend(false, GL_ONE, GL_ZERO);
}

/*************************************
 *  Vertex setup (from DOSBox-X voodoo_opengl.cpp)
 *************************************/

static void ogl_get_depth(voodoo_state* VV, INT32 ITERZ, INT64 ITERW, INT32 *depthval, INT32 *out_wfloat)
{
	INT32 wfloat;
	UINT32 FBZMODE = VV->reg[fbzMode].u;
	UINT32 FBZCOLORPATH = VV->reg[fbzColorPath].u;

	/* compute "floating point" W value (used for depth and fog) */
	if ((ITERW) & LONGTYPE(0xffff00000000))
		wfloat = 0x0000;
	else
	{
		UINT32 temp = (UINT32)(ITERW);
		if ((temp & 0xffff0000) == 0)
			wfloat = 0xffff;
		else
		{
			int exp = count_leading_zeros(temp);
			wfloat = ((exp << 12) | (INT32)(((~temp) >> (unsigned int)(19 - exp)) & 0xfffu));
			if (wfloat < 0xffff) wfloat++;
		}
	}

	/* compute depth value (W or Z) for this pixel */
	if (FBZMODE_WBUFFER_SELECT(FBZMODE) == 0)
		CLAMPED_Z(ITERZ, FBZCOLORPATH, *depthval);
	else if (FBZMODE_DEPTH_FLOAT_SELECT(FBZMODE) == 0)
		*depthval = wfloat;
	else
	{
		if ((unsigned int)(ITERZ) & 0xf0000000l)
			*depthval = 0x0000;
		else
		{
			UINT32 temp = (UINT32)((ITERZ) << 4);
			if ((temp & 0xffff0000ul) == 0)
				*depthval = 0xffff;
			else
			{
				int exp = count_leading_zeros(temp);
				*depthval = ((exp << 12) | (INT32)(((~temp) >> (unsigned int)(19 - exp)) & 0xfffu));
				if (*depthval < 0xffff) (*depthval)++;
			}
		}
	}

	/* add the bias */
	if (FBZMODE_ENABLE_DEPTH_BIAS(FBZMODE))
	{
		*depthval += (INT16)(VV)->reg[zaColor].u;
		CLAMP(*depthval, 0, 0xffff);
	}

	*out_wfloat = wfloat;
}

static void ogl_get_fog_blend(voodoo_state* v, INT32 wfloat, INT32 ITERZ, INT64 ITERW, INT32 *fogblend)
{
	UINT32 FOGMODE = v->reg[fogMode].u;
	UINT32 FBZCP = v->reg[fbzColorPath].u;

	*fogblend = 0;
	if (FOGMODE_ENABLE_FOG(FOGMODE) && !FOGMODE_FOG_CONSTANT(FOGMODE))
	{
		/* fog blending mode */
		switch (FOGMODE_FOG_ZALPHA(FOGMODE))
		{
		case 0:		/* fog table */
			{
				INT32 delta = v->fbi.fogdelta[wfloat >> 10];
				INT32 deltaval;

				/* perform the multiply against lower 8 bits of wfloat */
				deltaval = (delta & v->fbi.fogdelta_mask) *
					((wfloat >> 2) & 0xff);

				/* fog zones allow for negating this value */
				if (FOGMODE_FOG_ZONES(FOGMODE) && (delta & 2))
					deltaval = -deltaval;
				deltaval >>= 6;
				deltaval >>= 4;

				/* add to the blending factor */
				*fogblend = v->fbi.fogblend[wfloat >> 10] + deltaval;
				break;
			}

		case 1:		/* iterated A, applied per vertex by the caller */
			break;

		case 2:		/* iterated Z */
			CLAMPED_Z((ITERZ), FBZCP, *fogblend);
			*fogblend >>= 8;
			break;

		case 3:		/* iterated W - Voodoo 2 only */
			CLAMPED_W((ITERW), FBZCP, *fogblend);
			break;
		}
	}
}

static void ogl_get_vertex_data(INT32 x, INT32 y, const void *extradata, ogl_vertex_data *vd) {
	const poly_extra_data *extra = (const poly_extra_data *)extradata;
	v=(voodoo_state*)extra->state;
	INT32 iterr, iterg, iterb, itera;
	INT32 iterz;
	INT64 iterw;
	INT32 dx, dy;
	INT32 d;

	dx = x - extra->ax;
	dy = y - extra->ay;
	iterr = extra->startr + ((dy * extra->drdy)>>4) + ((dx * extra->drdx)>>4);
	iterg = extra->startg + ((dy * extra->dgdy)>>4) + ((dx * extra->dgdx)>>4);
	iterb = extra->startb + ((dy * extra->dbdy)>>4) + ((dx * extra->dbdx)>>4);
	itera = extra->starta + ((dy * extra->dady)>>4) + ((dx * extra->dadx)>>4);
	iterz = extra->startz + ((dy * extra->dzdy)>>4) + ((dx * extra->dzdx)>>4);
	iterw = extra->startw + ((dy * extra->dwdy)>>4) + ((dx * extra->dwdx)>>4);

	for (Bitu i=0; i<extra->texcount; i++) {
		INT64 iters,itert,iterw;
		UINT32 smax,tmax;
		INT64 s, t;
		INT32 lod=0;

		UINT32 texmode = (UINT32)(i == 0 ? extra->r_textureMode0 : extra->r_textureMode1);
		INT32 LODBASE = (i==0) ? extra->lodbase0 : extra->lodbase1;

		UINT32 ilod = (UINT32)(v->tmu[i].lodmin >> 8);
		if (!((v->tmu[i].lodmask >> ilod) & 1))
			ilod++;

		smax = (v->tmu[i].wmask >> ilod) + 1;
		tmax = (v->tmu[i].hmask >> ilod) + 1;

		iterw = v->tmu[i].startw + ((dy * v->tmu[i].dwdy)>>4) + ((dx * v->tmu[i].dwdx)>>4);
		iters = v->tmu[i].starts + ((dy * v->tmu[i].dsdy)>>4) + ((dx * v->tmu[i].dsdx)>>4);
		itert = v->tmu[i].startt + ((dy * v->tmu[i].dtdy)>>4) + ((dx * v->tmu[i].dtdx)>>4);

		/* determine the S/T/LOD values for this texture */
		if (TEXMODE_ENABLE_PERSPECTIVE(texmode))
		{
			INT64 oow = fast_reciplog((iterw), &lod);
			s = (oow * (iters)) >> 29;
			t = (oow * (itert)) >> 29;
			lod += LODBASE;
		}
		else
		{
			s = (iters) >> 14;
			t = (itert) >> 14;
			lod = LODBASE;
		}

		/* clamp the LOD */
		lod += v->tmu[i].lodbias;
		if (lod < v->tmu[i].lodmin)
			lod = v->tmu[i].lodmin;
		if (lod > v->tmu[i].lodmax)
			lod = v->tmu[i].lodmax;

		/* clamp W */
		if (TEXMODE_CLAMP_NEG_W(texmode) && (iterw) < 0)
			s = t = 0;

		if (s != 0) vd->m[i].s = (float)((float)s/(float)(smax*(1u<<(18u+ilod))));
		else vd->m[i].s = 0.0f;
		if (t != 0) vd->m[i].t = (float)((float)t/(float)(tmax*(1u<<(18u+ilod))));
		else vd->m[i].t = 0.0f;
		if (iterw != 0) vd->m[i].w = (float)((float)iterw/(float)(0xffffff));
		else vd->m[i].w = 0.0f;

		vd->m[i].sw = vd->m[i].s * vd->m[i].w;
		vd->m[i].tw = vd->m[i].t * vd->m[i].w;
		vd->m[i].z  = 0.0f;
		vd->m[i].lodblend = 0.0f;
	}

	vd->r = (float)((float)iterr/(float)(1<<20));
	vd->g = (float)((float)iterg/(float)(1<<20));
	vd->b = (float)((float)iterb/(float)(1<<20));
	vd->a = (float)((float)itera/(float)(1<<20));
	vd->z = (float)((float)iterz/(float)(1<<20));
	vd->w = (float)((float)iterw/(float)(0xffffff));

	INT32 wfloat;
	ogl_get_depth(v,iterz,iterw,&d,&wfloat);
	vd->d = (float)((float)d/(float)(0xffff));

	INT32 fogblend;
	ogl_get_fog_blend(v,wfloat,iterz,iterw,&fogblend);
	vd->fogblend = (float)fogblend/255.0f;

	// OpenGL-correction for Blood/ShadowWarrior
	vd->x = (((float)x) - (1.0f/16.0f)) / 16.0f;
	vd->y = (((float)y) - (1.0f/16.0f)) / 16.0f;
}

/*************************************
 *  Texture cache (from DOSBox-X voodoo_opengl.cpp)
 *
 *  Converted to RGBA bytes and uploaded without mipmaps (MiniGL has no
 *  mipmap generation; the classic Voodoo titles look right at LOD 0).
 *  Chroma keying on texels is part of a texture's identity: matching texels
 *  get alpha 0 and the draw adds an alpha test.
 *************************************/

#define MGL_NO_CHROMA 0xffffffffu

struct mgl_texmap {
	bool valid_pal;
	UINT32 format;
	GLuint current_id;
	std::map<UINT32, GLuint>* ids;	/* palette checksum -> texture, palettized formats */
	UINT32 width;
	UINT32 height;
	UINT32 ilod;
	UINT32 lodmask;
	UINT32 ncc_table;
	UINT32 chroma;
};

static std::map<UINT32, mgl_texmap> textures[2];
static UINT32 texture_count = 0;
/* 256x256 RGBA, the largest Voodoo texture */
static UINT8 texrgba[256*256*4];

static UINT32 crc_32_tab[256];

static void mgl_init_crc(void) {
	for (UINT32 n = 0; n < 256; n++) {
		UINT32 c = n;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		crc_32_tab[n] = c;
	}
}

static bool ogl_palette_format(UINT32 format) {
	return format == 1 || format == 9 || format == 5 || format == 14;
}

static UINT32 calculate_palsum(UINT32 tmunum, UINT32 texmode) {
	const UINT32 format = TEXMODE_FORMAT(texmode);
	const rgb_t *colors = (format & 7) == 1
		? v->tmu[tmunum].ncc[TEXMODE_NCC_TABLE_SELECT(texmode)].texel
		: v->tmu[tmunum].palette;
	UINT32 csum = 0;
	for (Bitu pct=0; pct<256; pct++) {
		UINT32 pval = colors[pct];
		csum = crc_32_tab[(csum ^ pval) & 0xff] ^ (csum>>8);
		csum = crc_32_tab[(csum ^ (pval>>8)) & 0xff] ^ (csum>>8);
		csum = crc_32_tab[(csum ^ (pval>>16)) & 0xff] ^ (csum>>8);
		csum = crc_32_tab[(csum ^ (pval>>24)) & 0xff] ^ (csum>>8);
	}
	return csum;
}

static void mgl_delete_texture(GLuint id) {
	mgl_end();
	for (int u = 0; u < 2; u++)
		if (gs.tex_bound[u] == id) gs.tex_bound[u] = 0;
	glDeleteTextures(1, &id);
	if (texture_count) texture_count--;
}

static void ogl_destroy_cached_texture(mgl_texmap &texture) {
	if (texture.ids != NULL) {
		for (std::map<UINT32, GLuint>::iterator i = texture.ids->begin(); i != texture.ids->end(); ++i)
			mgl_delete_texture(i->second);
		delete texture.ids;
		texture.ids = NULL;
	} else if (texture.current_id) {
		mgl_delete_texture(texture.current_id);
	}
	texture.current_id = 0;
}

static void mgl_flush_all_textures(void) {
	for (int j = 0; j < 2; j++) {
		for (std::map<UINT32, mgl_texmap>::iterator t = textures[j].begin(); t != textures[j].end(); ++t)
			ogl_destroy_cached_texture(t->second);
		textures[j].clear();
	}
	texture_count = 0;
}

/* Writes only accumulate a conservative byte interval. Visit cached textures
 * once before the next draw. */
struct ogl_dirty_range {
	bool pending;
	UINT32 first;
	UINT32 last;
};
static ogl_dirty_range texture_dirty[2];

static void ogl_flush_texture_writes(int TMU) {
	ogl_dirty_range &dirty = texture_dirty[TMU];
	if (!dirty.pending) return;
	const UINT32 mask = v->tmu[TMU].mask;
	for (std::map<UINT32, mgl_texmap>::iterator t = textures[TMU].begin(); t != textures[TMU].end();) {
		const UINT32 size = t->second.width * t->second.height *
			(t->second.format < 8 ? 1 : 2);
		const UINT32 last = t->first + size - 1;
		const bool overlaps = last <= mask
			? t->first <= dirty.last && last >= dirty.first
			: t->first <= dirty.last || (last & mask) >= dirty.first;
		if (overlaps) {
			ogl_destroy_cached_texture(t->second);
			textures[TMU].erase(t++);
		} else {
			++t;
		}
	}
	dirty.pending = false;
}

/* Too many textures for the card: start over rather than fail uploads. */
#define MGL_MAX_TEXTURES 1536

static GLuint mgl_upload_texture(UINT32 j, UINT32 TEXMODE, UINT32 texbase,
                                 UINT32 width, UINT32 height, UINT32 chroma) {
	const UINT32 format = TEXMODE_FORMAT(TEXMODE);
	const UINT32 count = width*height;
	const tmu_state &tmu = v->tmu[j];
	UINT32 texboffset = texbase;
	UINT8 *out = texrgba;

	for (UINT32 i = 0; i < count; i++) {
		UINT32 data;
		if (format < 8) {
			data = tmu.lookup[tmu.ram[texboffset & tmu.mask]];
			texboffset++;
		} else {
			const UINT16 texel = *(const UINT16 *)&tmu.ram[texboffset & tmu.mask];
			if (format >= 10 && format <= 12)
				data = tmu.lookup[texel];
			else	/* 8-bit colour + 8-bit alpha */
				data = (tmu.lookup[texel & 0xff] & 0xffffff) | ((UINT32)(texel & 0xff00u) << 16);
			texboffset += 2;
		}
		/* data is 0xAARRGGBB */
		out[0] = (UINT8)(data >> 16);
		out[1] = (UINT8)(data >> 8);
		out[2] = (UINT8)data;
		out[3] = (chroma != MGL_NO_CHROMA && (data & 0xffffff) == chroma) ? 0 : (UINT8)(data >> 24);
		out += 4;
	}

	if (texture_count >= MGL_MAX_TEXTURES) {
		LOG_MSG("VOODOO: MiniGL texture cache full, flushing");
		mgl_flush_all_textures();
	}

	GLuint texID = 0;
	mgl_end();
	glGenTextures(1, &texID);
	texture_count++;
	mgl_select_unit(0);
	glBindTexture(GL_TEXTURE_2D, texID);
	gs.tex_bound[0] = texID;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texrgba);
	return texID;
}

/* Returns the GL texture for TMU j in the current state, 0 if none. */
static GLuint ogl_cache_texture(const poly_extra_data *extra, UINT32 j, UINT32 chroma) {
	ogl_flush_texture_writes((int)j);
	if (j >= extra->texcount || v->tmu[j].ram == NULL || v->tmu[j].lodmin >= (8 << 8)) return 0;

	const UINT32 TEXMODE = (UINT32)(j==0 ? extra->r_textureMode0 : extra->r_textureMode1);
	const UINT32 format = TEXMODE_FORMAT(TEXMODE);
	const UINT32 ncc_table = (format & 7) == 1 ? TEXMODE_NCC_TABLE_SELECT(TEXMODE) : 0;

	UINT32 ilod = (UINT32)(v->tmu[j].lodmin >> 8);
	if (!((v->tmu[j].lodmask >> ilod) & 1))
		ilod++;
	const UINT32 texbase = v->tmu[j].lodoffset[ilod];
	const UINT32 width = (v->tmu[j].wmask >> ilod) + 1;
	const UINT32 height = (v->tmu[j].hmask >> ilod) + 1;
	if (width > 256 || height > 256) return 0;

	std::map<UINT32, mgl_texmap>::iterator t = textures[j].find(texbase);
	// A RAM address can be reused with a different texture layout.
	if (t != textures[j].end() &&
	    (t->second.format != format || t->second.width != width ||
	     t->second.height != height || t->second.ilod != ilod ||
	     t->second.lodmask != v->tmu[j].lodmask || t->second.ncc_table != ncc_table ||
	     t->second.chroma != chroma)) {
		ogl_destroy_cached_texture(t->second);
		textures[j].erase(t);
		t = textures[j].end();
	}

	if (t != textures[j].end()) {
		if (t->second.ids == NULL || t->second.valid_pal)
			return t->second.current_id;
		/* palette changed: find or build the texture for the new palette */
		const UINT32 psum = calculate_palsum(j, TEXMODE);
		std::map<UINT32, GLuint>::iterator u = t->second.ids->find(psum);
		t->second.valid_pal = true;
		if (u != t->second.ids->end()) {
			t->second.current_id = u->second;
			return u->second;
		}
		if (t->second.ids->size() > 8) {
			for (u = t->second.ids->begin(); u != t->second.ids->end(); ++u)
				mgl_delete_texture(u->second);
			t->second.ids->clear();
		}
		const GLuint texID = mgl_upload_texture(j, TEXMODE, texbase, width, height, chroma);
		(*t->second.ids)[psum] = texID;
		t->second.current_id = texID;
		return texID;
	}

	const GLuint texID = mgl_upload_texture(j, TEXMODE, texbase, width, height, chroma);
	mgl_texmap tex;
	tex.valid_pal = true;
	tex.format = format;
	tex.current_id = texID;
	tex.ids = NULL;
	tex.width = width;
	tex.height = height;
	tex.ilod = ilod;
	tex.lodmask = v->tmu[j].lodmask;
	tex.ncc_table = ncc_table;
	tex.chroma = chroma;
	if (ogl_palette_format(format)) {
		tex.ids = new std::map<UINT32, GLuint>();
		(*tex.ids)[calculate_palsum(j, TEXMODE)] = texID;
	}
	textures[j][texbase] = tex;
	return texID;
}

void voodoo_ogl_invalidate_paltex(void) {
	for (int j=0; j<2; j++) {
		for (std::map<UINT32, mgl_texmap>::iterator t=textures[j].begin(); t!=textures[j].end(); ++t) {
			if (ogl_palette_format(t->second.format))
				t->second.valid_pal = false;
		}
	}
}

void voodoo_ogl_texture_clear(UINT32 address, int TMU) {
	if (textures[TMU].empty()) return;
	const UINT32 mask = v->tmu[TMU].mask;
	UINT32 first = address & mask;
	UINT32 last = (first + 3) & mask;
	// A wrapped dword conservatively dirties the whole circular address space.
	if (last < first) {
		first = 0;
		last = mask;
	}
	ogl_dirty_range &dirty = texture_dirty[TMU];
	if (!dirty.pending) {
		dirty.first = first;
		dirty.last = last;
		dirty.pending = true;
	} else {
		if (first < dirty.first) dirty.first = first;
		if (last > dirty.last) dirty.last = last;
	}
}

static void mgl_set_texture_params(UINT32 TEXMODE) {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TEXMODE_MINIFICATION_FILTER(TEXMODE) ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TEXMODE_MAGNIFICATION_FILTER(TEXMODE) ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, TEXMODE_CLAMP_S(TEXMODE) ? GL_CLAMP : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, TEXMODE_CLAMP_T(TEXMODE) ? GL_CLAMP : GL_REPEAT);
}

/*************************************
 *  Combine units -> fixed function
 *
 *  Every SST-1 combine unit computes
 *     (zero_other ? 0 : other) - (sub_clocal ? local : 0)
 *     times f (mselect; inverted unless reverse_blend)
 *     plus (add: 0, local or local alpha)
 *  per channel group (RGB and alpha).
 *************************************/

enum {
	MGL_SRC_ZERO = 0,	/* constant 0 */
	MGL_SRC_ONE,		/* constant 1 */
	MGL_SRC_ITER,		/* iterated RGB / A */
	MGL_SRC_ITERA,		/* iterated A (as RGB factor) */
	MGL_SRC_COLOR0,
	MGL_SRC_COLOR0A,
	MGL_SRC_COLOR1,
	MGL_SRC_COLOR1A,
	MGL_SRC_TEX,		/* the (TMU-combined) texel */
	MGL_SRC_TEXA		/* its alpha */
};

struct mgl_combine {
	int other, local, factor;	/* MGL_SRC_* */
	bool zero_other, sub_local, invert_factor;
	int add;			/* MGL_SRC_* */
};

/* f: mselect 0 zero, 1 c_local, 2 a_other, 3 a_local, 4 texture alpha
 * (5 texture RGB, Voodoo 2 only: treated as texture alpha) */
static int mgl_rgb_factor(int mselect, int local, int other_alpha, int local_alpha) {
	switch (mselect) {
	case 1: return local;
	case 2: return other_alpha;
	case 3: return local_alpha;
	case 4: case 5: return MGL_SRC_TEXA;
	default: return MGL_SRC_ZERO;
	}
}

/* value of a non-texture source at one vertex, channel c (0..2 RGB, 3 A) */
static float mgl_source_value(int src, const ogl_vertex_data &vd, int c) {
	const voodoo_reg &c0 = v->reg[color0];
	const voodoo_reg &c1 = v->reg[color1];
	switch (src) {
	case MGL_SRC_ONE: return 1.0f;
	case MGL_SRC_ITER: return c == 0 ? vd.r : c == 1 ? vd.g : c == 2 ? vd.b : vd.a;
	case MGL_SRC_ITERA: return vd.a;
	case MGL_SRC_COLOR0: return (c == 0 ? c0.rgb.r : c == 1 ? c0.rgb.g : c == 2 ? c0.rgb.b : c0.rgb.a) / 255.0f;
	case MGL_SRC_COLOR0A: return c0.rgb.a / 255.0f;
	case MGL_SRC_COLOR1: return (c == 0 ? c1.rgb.r : c == 1 ? c1.rgb.g : c == 2 ? c1.rgb.b : c1.rgb.a) / 255.0f;
	case MGL_SRC_COLOR1A: return c1.rgb.a / 255.0f;
	default: return 0.0f;
	}
}

static bool mgl_is_tex(int src) {
	return src == MGL_SRC_TEX || src == MGL_SRC_TEXA;
}

/* How the texel enters a combine result. */
enum {
	MGL_TEXUSE_NONE,	/* result = C, C computed per vertex */
	MGL_TEXUSE_MODULATE,	/* result = T * C */
	MGL_TEXUSE_DECAL	/* result = C + (T - C) * Ta */
};

/* Reduce a combine to a texture use plus the per-vertex colour channel. */
static int mgl_combine_vertex(const mgl_combine &cb, const ogl_vertex_data &vd, int c, float *out) {
	const bool other_tex = !cb.zero_other && mgl_is_tex(cb.other);
	const bool factor_tex = mgl_is_tex(cb.factor);

	/* non-texture factor value (1 - f when inverted) */
	float f = factor_tex ? 1.0f : mgl_source_value(cb.factor, vd, c);
	if (cb.invert_factor && !factor_tex) f = 1.0f - f;
	const float local = mgl_source_value(cb.local, vd, c);
	const float add = (cb.add == MGL_SRC_ZERO) ? 0.0f : mgl_source_value(cb.add, vd, c);

	if (!other_tex) {
		float o = cb.zero_other ? 0.0f : mgl_source_value(cb.other, vd, c);
		if (cb.sub_local) o -= local;
		if (factor_tex) {
			/* f is the texel alpha: (o)*Ta + add; only o==local, add==local
			 * style lerps are exact (decal toward zero) - approximate with o */
			*out = o + add;
			return MGL_TEXUSE_NONE;
		}
		*out = o * f + add;
		return MGL_TEXUSE_NONE;
	}

	/* other is the texel */
	if (cb.sub_local && factor_tex && !cb.invert_factor && cb.add != MGL_SRC_ZERO) {
		/* (T - L) * Ta + L: the classic decal */
		*out = local;
		return MGL_TEXUSE_DECAL;
	}
	if (factor_tex) {
		/* T * Ta (or 1 - Ta): keep the texel */
		*out = 1.0f;
		return MGL_TEXUSE_MODULATE;
	}
	/* T * f (+ dropped constant terms when sub/add are used) */
	*out = f;
	return MGL_TEXUSE_MODULATE;
}

static void mgl_decode_fbz_combine(UINT32 FBZCP, mgl_combine &rgb, mgl_combine &alpha) {
	static const int other_rgb[4] = { MGL_SRC_ITER, MGL_SRC_TEX, MGL_SRC_COLOR1, MGL_SRC_ZERO };
	static const int other_a[4] = { MGL_SRC_ITERA, MGL_SRC_TEXA, MGL_SRC_COLOR1A, MGL_SRC_ZERO };
	const int a_other = other_a[FBZCP_CC_ASELECT(FBZCP)];
	int a_local;
	switch (FBZCP_CCA_LOCALSELECT(FBZCP)) {
	case 1: a_local = MGL_SRC_COLOR0A; break;
	case 0: default: a_local = MGL_SRC_ITERA; break;	/* 2: iterated Z, rare */
	}

	rgb.other = other_rgb[FBZCP_CC_RGBSELECT(FBZCP)];
	/* local select override by texel alpha bit 7 is not representable; use color0 */
	rgb.local = (FBZCP_CC_LOCALSELECT(FBZCP) || FBZCP_CC_LOCALSELECT_OVERRIDE(FBZCP)) ? MGL_SRC_COLOR0 : MGL_SRC_ITER;
	rgb.zero_other = FBZCP_CC_ZERO_OTHER(FBZCP) != 0;
	rgb.sub_local = FBZCP_CC_SUB_CLOCAL(FBZCP) != 0;
	rgb.factor = mgl_rgb_factor(FBZCP_CC_MSELECT(FBZCP), rgb.local, a_other,
	                            a_local == MGL_SRC_COLOR0A ? MGL_SRC_COLOR0A : MGL_SRC_ITERA);
	rgb.invert_factor = !FBZCP_CC_REVERSE_BLEND(FBZCP);
	rgb.add = FBZCP_CC_ADD_ACLOCAL(FBZCP) == 1 ? rgb.local :
	          FBZCP_CC_ADD_ACLOCAL(FBZCP) == 2 ? (a_local == MGL_SRC_COLOR0A ? MGL_SRC_COLOR0A : MGL_SRC_ITERA) :
	          MGL_SRC_ZERO;

	alpha.other = a_other;
	alpha.local = a_local;
	alpha.zero_other = FBZCP_CCA_ZERO_OTHER(FBZCP) != 0;
	alpha.sub_local = FBZCP_CCA_SUB_CLOCAL(FBZCP) != 0;
	alpha.factor = mgl_rgb_factor(FBZCP_CCA_MSELECT(FBZCP), a_local, a_other, a_local);
	alpha.invert_factor = !FBZCP_CCA_REVERSE_BLEND(FBZCP);
	alpha.add = FBZCP_CCA_ADD_ACLOCAL(FBZCP) ? a_local : MGL_SRC_ZERO;
}

/* What TMU0's combine unit makes of TMU0 (local) and TMU1 (other). */
enum {
	MGL_TMU_LOCAL,		/* TMU0 texel */
	MGL_TMU_OTHER,		/* TMU1 texel passed through */
	MGL_TMU_PRODUCT		/* TMU1 * TMU0 */
};

static int mgl_decode_tmu0_combine(UINT32 TEXMODE) {
	if (TEXMODE_TC_ZERO_OTHER(TEXMODE) || TEXMODE_TC_SUB_CLOCAL(TEXMODE)) return MGL_TMU_LOCAL;
	const int mselect = TEXMODE_TC_MSELECT(TEXMODE);
	const bool reverse = TEXMODE_TC_REVERSE_BLEND(TEXMODE) != 0;
	if (mselect == 0 && !reverse && TEXMODE_TC_ADD_ACLOCAL(TEXMODE) == 0) return MGL_TMU_OTHER;
	if (mselect == 1 && reverse) return MGL_TMU_PRODUCT;
	return MGL_TMU_LOCAL;
}

/*************************************
 *  Triangles
 *************************************/

void voodoo_ogl_draw_triangle(poly_extra_data *extra) {
	v=extra->state;
	ogl_vertex_data vd[3];

	const UINT32 ALPHAMODE = extra->r_alphaMode;
	const UINT32 FBZMODE   = extra->r_fbzMode;
	const UINT32 FBZCP     = extra->r_fbzColorPath;
	const UINT32 FOGMODE   = extra->r_fogMode;

	ogl_get_vertex_data(v->fbi.ax, v->fbi.ay, (void*)extra, &vd[0]);
	ogl_get_vertex_data(v->fbi.bx, v->fbi.by, (void*)extra, &vd[1]);
	ogl_get_vertex_data(v->fbi.cx, v->fbi.cy, (void*)extra, &vd[2]);

	for (int i = 0; i < 3; i++) {
		vd[i].x = (vd[i].x * new_width) / v->fbi.width;
		vd[i].y = (vd[i].y * new_height) / v->fbi.height;
	}

	mgl_combine rgb, alpha;
	mgl_decode_fbz_combine(FBZCP, rgb, alpha);
	const bool uses_tex = FBZCP_TEXTURE_ENABLE(FBZCP) && extra->texcount > 0 &&
		((!rgb.zero_other && mgl_is_tex(rgb.other)) || mgl_is_tex(rgb.factor) ||
		 (!alpha.zero_other && mgl_is_tex(alpha.other)) || mgl_is_tex(alpha.factor));

	/* texture setup: which TMU texels feed GL units 0 and 1 */
	GLuint tex[2] = { 0, 0 };
	UINT32 texmode[2] = { (UINT32)extra->r_textureMode0, (UINT32)extra->r_textureMode1 };
	int tmu_of_unit[2] = { 0, 1 };
	bool chroma_tex = false;
	if (uses_tex) {
		/* chroma key against the texel: fold it into the texture alpha */
		UINT32 chroma = MGL_NO_CHROMA;
		if (FBZMODE_ENABLE_CHROMAKEY(FBZMODE) && rgb.other == MGL_SRC_TEX) {
			chroma = v->reg[chromaKey].u & 0xffffff;
			chroma_tex = true;
		}
		int tmu_mode = MGL_TMU_LOCAL;
		if (extra->texcount > 1 && v->tmu[1].ram != NULL)
			tmu_mode = mgl_decode_tmu0_combine(texmode[0]);
		switch (tmu_mode) {
		case MGL_TMU_OTHER:
			tex[0] = ogl_cache_texture(extra, 1, chroma);
			tmu_of_unit[0] = 1;
			break;
		case MGL_TMU_PRODUCT:
			tex[0] = ogl_cache_texture(extra, 1, chroma);
			tex[1] = ogl_cache_texture(extra, 0, MGL_NO_CHROMA);
			tmu_of_unit[0] = 1;
			tmu_of_unit[1] = 0;
			if (!tex[0]) {	/* nothing in TMU1: plain TMU0 */
				tex[0] = tex[1];
				tex[1] = 0;
				tmu_of_unit[0] = 0;
			}
			break;
		default:
			tex[0] = ogl_cache_texture(extra, 0, chroma);
			break;
		}
	}

	/* per-vertex colour and texture use */
	float col[3][4];
	int use_rgb = MGL_TEXUSE_NONE, use_a = MGL_TEXUSE_NONE;
	for (int i = 0; i < 3; i++) {
		for (int c = 0; c < 3; c++)
			use_rgb = mgl_combine_vertex(rgb, vd[i], c, &col[i][c]);
		use_a = mgl_combine_vertex(alpha, vd[i], 3, &col[i][3]);
		if (FBZCP_CC_INVERT_OUTPUT(FBZCP) && use_rgb == MGL_TEXUSE_NONE)
			for (int c = 0; c < 3; c++) col[i][c] = 1.0f - col[i][c];
		if (FBZCP_CCA_INVERT_OUTPUT(FBZCP) && use_a == MGL_TEXUSE_NONE)
			col[i][3] = 1.0f - col[i][3];

		/* fog toward fogColor; exact for untextured or black fog */
		if (FOGMODE_ENABLE_FOG(FOGMODE)) {
			float f = FOGMODE_FOG_CONSTANT(FOGMODE) ? 0.0f :
			          (FOGMODE_FOG_ZALPHA(FOGMODE) == 1 ? vd[i].a : vd[i].fogblend);
			if (f < 0.0f) f = 0.0f;
			if (f > 1.0f) f = 1.0f;
			const voodoo_reg &fc = v->reg[fogColor];
			const float fogc[3] = { fc.rgb.r/255.0f, fc.rgb.g/255.0f, fc.rgb.b/255.0f };
			for (int c = 0; c < 3; c++) {
				col[i][c] *= 1.0f - f;
				if (use_rgb == MGL_TEXUSE_NONE) col[i][c] += fogc[c] * f;
			}
		}
		for (int c = 0; c < 4; c++) {
			if (col[i][c] < 0.0f) col[i][c] = 0.0f;
			if (col[i][c] > 1.0f) col[i][c] = 1.0f;
		}
	}
	if (!tex[0]) use_rgb = use_a = MGL_TEXUSE_NONE;
	/* An alpha computed without the texel still goes through GL_MODULATE and
	 * gets multiplied by the texel alpha, which is 1 for formats without
	 * alpha; GL_DECAL keeps the vertex alpha. */
	const bool textured = use_rgb != MGL_TEXUSE_NONE || use_a != MGL_TEXUSE_NONE;

	/* state */
	mgl_apply_depth_state(FBZMODE);
	mgl_apply_blend_state(ALPHAMODE);
	if (ALPHAMODE_ALPHATEST(ALPHAMODE))
		mgl_set_alpha_test(true, GL_NEVER + ALPHAMODE_ALPHAFUNCTION(ALPHAMODE), v->reg[alphaMode].rgb.a / 255.0f);
	else if (chroma_tex && textured)
		mgl_set_alpha_test(true, GL_GREATER, 0.0f);
	else
		mgl_set_alpha_test(false, GL_ALWAYS, 0.0f);
	mgl_set_draw_front(FBZMODE_DRAW_BUFFER(v->reg[fbzMode].u) == 0);

	if (textured) {
		const GLenum env0 = (use_rgb == MGL_TEXUSE_DECAL) ? GL_DECAL : GL_MODULATE;
		if (tex[1]) {
			mgl_set_texture(1, tex[1], GL_MODULATE);
			mgl_select_unit(1);
			mgl_set_texture_params(texmode[tmu_of_unit[1]]);
		} else {
			mgl_set_texture(1, 0, 0);
		}
		mgl_set_texture(0, tex[0], env0);
		mgl_select_unit(0);
		mgl_set_texture_params(texmode[tmu_of_unit[0]]);
	} else {
		mgl_untextured();
	}

	/* perspective from the homogeneous vertex: w = 1/oow */
	const int wtmu = textured ? tmu_of_unit[0] : -1;
	const bool perspective = textured && TEXMODE_ENABLE_PERSPECTIVE(texmode[wtmu]);

	mgl_begin(GL_TRIANGLES);
	for (int i = 0; i < 3; i++) {
		glColor4f(col[i][0], col[i][1], col[i][2], col[i][3]);
		GLfloat W = 1.0f;
		if (perspective) {
			const float oow = vd[i].m[wtmu].w;
			W = (oow > 1.0e-6f) ? 1.0f / oow : 1.0e6f;
		}
		if (textured) {
			if (tex[1]) {
				glMultiTexCoord2fARB(GL_TEXTURE0_ARB, vd[i].m[tmu_of_unit[0]].s, vd[i].m[tmu_of_unit[0]].t);
				glMultiTexCoord2fARB(GL_TEXTURE1_ARB, vd[i].m[tmu_of_unit[1]].s, vd[i].m[tmu_of_unit[1]].t);
			} else {
				glTexCoord2f(vd[i].m[wtmu].s, vd[i].m[wtmu].t);
			}
		}
		glVertex4f(vd[i].x * W, vd[i].y * W, vd[i].d * W, W);
	}
}

/*************************************
 *  LFB access in accelerated mode
 *************************************/

static void mgl_lfb_target(void) {
	mgl_untextured();
	mgl_set_alpha_test(false, GL_ALWAYS, 0.0f);
	mgl_set_draw_front(LFBMODE_WRITE_BUFFER_SELECT(v->reg[lfbMode].u) == 0);
}

static void mgl_points(int x, int y, GLfloat z, bool with_z) {
	const float dx = (float)new_width / (float)v->fbi.width;
	const float dy = (float)new_height / (float)v->fbi.height;
	const int startx = (int)(x*dx), endx = (int)((x + 1)*dx);
	const int starty = (int)(y*dy), endy = (int)((y + 1)*dy);
	mgl_begin(GL_POINTS);
	for (int nx = startx; nx < endx; nx++)
		for (int ny = starty; ny < endy; ny++) {
			if (with_z) glVertex3f(nx + 0.5f, ny + 0.5f, z);
			else glVertex2f(nx + 0.5f, ny + 0.5f);
		}
}

void voodoo_ogl_draw_pixel(int x, int y, bool has_rgb, bool has_alpha, int r, int g, int b, int a) {
	mgl_lfb_target();
	mgl_set_depth(false, GL_ALWAYS);
	mgl_set_depth_mask(false);
	mgl_set_color_mask(has_rgb);
	mgl_set_blend(false, GL_ONE, GL_ZERO);
	if (!has_rgb) return;
	mgl_begin(GL_POINTS);
	glColor4ub((GLubyte)(r&0xff), (GLubyte)(g&0xff), (GLubyte)(b&0xff), (GLubyte)(a&0xff));
	mgl_points(x, y, 0.0f, false);
}

void voodoo_ogl_draw_z(int x, int y, int z) {
	mgl_lfb_target();
	mgl_set_depth(true, GL_ALWAYS);
	mgl_set_depth_mask(true);
	mgl_set_color_mask(false);
	mgl_set_blend(false, GL_ONE, GL_ZERO);
	mgl_points(x, y, (GLfloat)(z & 0xffff) / 65535.0f, true);
}

void voodoo_ogl_draw_pixel_pipeline(int x, int y, int r, int g, int b) {
	mgl_lfb_target();
	mgl_set_depth(false, GL_ALWAYS);
	mgl_set_depth_mask(false);
	mgl_set_color_mask(true);
	mgl_apply_blend_state(v->reg[alphaMode].u);
	mgl_begin(GL_POINTS);
	glColor3ub((GLubyte)(r&0xff), (GLubyte)(g&0xff), (GLubyte)(b&0xff));
	mgl_points(x, y, 0.0f, false);
}

UINT32 voodoo_ogl_read_pixel(int x, int y) {
	if ((x < 0) || (y < 0) || (x >= (INT32)v->fbi.width) || (y >= (INT32)v->fbi.height)) return 0xffffffff;
	const UINT32 buf = LFBMODE_READ_BUFFER_SELECT(v->reg[lfbMode].u);
	if (buf > 1) return 0xffffffff;	/* aux (depth) readback is not available */
	x = (x * (int)new_width)  / (int)v->fbi.width;
	y = (y * (int)new_height) / (int)v->fbi.height;
	if (x + 1 >= (int)new_width) x = (int)new_width - 2;

	if (cached_line_len < new_width) {
		for (int i = 0; i < 2; i++) {
			free(cached_line[i]);
			cached_line[i] = (UINT8 *)malloc(new_width * 4);
			cached_line_y[i] = -1;
		}
		cached_line_len = new_width;
	}
	if (!cached_line[buf]) return 0xffffffff;
	if (cached_line_y[buf] != y) {
		mgl_end();
		glReadBuffer(buf == 0 ? GL_FRONT : GL_BACK);
		/* GL rows count from the bottom */
		glReadPixels(0, (GLint)new_height - 1 - y, (GLsizei)new_width, 1, GL_RGBA, GL_UNSIGNED_BYTE, cached_line[buf]);
		cached_line_y[buf] = y;
	}
	const UINT8 *p0 = cached_line[buf] + x*4;
	const UINT8 *p1 = p0 + 4;
	return ((UINT32)(p0[0]>>3)<<11) | ((UINT32)(p0[1]>>2)<<5) | (UINT32)(p0[2]>>3) |
	       ((UINT32)(p1[0]>>3)<<27) | ((UINT32)(p1[1]>>2)<<21) | ((UINT32)(p1[2]>>3)<<16);
}

/*************************************
 *  Clears, swaps, window
 *************************************/

void voodoo_ogl_clip_window(voodoo_state *v) {
	/* triangles arrive clipped by the core's setup; like DOSBox-X, no scissor */
}

void voodoo_ogl_fastfill(void) {
	mgl_end();

	int sx = (v->reg[clipLeftRight].u >> 16) & 0x3ff;
	int ex = (v->reg[clipLeftRight].u >> 0) & 0x3ff;
	int sy = (v->reg[clipLowYHighY].u >> 16) & 0x3ff;
	int ey = (v->reg[clipLowYHighY].u >> 0) & 0x3ff;

	{
		int tmp = ((int)v->fbi.yorigin+1 - ey) & 0x3ff;
		ey = ((int)v->fbi.yorigin+1 - sy) & 0x3ff;
		sy = tmp;
	}

	bool scissors_needed = true;
	if ((sx == 0) && (sy == 0) && ((Bitu)ex == (Bitu)v->fbi.width) && ((Bitu)ey == (Bitu)v->fbi.height))
		scissors_needed = false;

	sx = (sx * (int)new_width) / (int)v->fbi.width;
	ex = (ex * (int)new_width) / (int)v->fbi.width;
	sy = (sy * (int)new_height) / (int)v->fbi.height;
	ey = (ey * (int)new_height) / (int)v->fbi.height;

	if (scissors_needed) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(sx, sy, ex-sx, ey-sy);
	}

	GLbitfield clear_mask = 0;
	if (FBZMODE_RGB_BUFFER_MASK(v->reg[fbzMode].u)) {
		clear_mask |= GL_COLOR_BUFFER_BIT;
		mgl_set_color_mask(true);
		if (last_clear_color != v->reg[color1].u) {
			glClearColor((float)v->reg[color1].rgb.r/255.0f,
				(float)v->reg[color1].rgb.g/255.0f,
				(float)v->reg[color1].rgb.b/255.0f,
				(float)v->reg[color1].rgb.a/255.0f);
			last_clear_color = v->reg[color1].u;
		}
		mgl_set_draw_front(FBZMODE_DRAW_BUFFER(v->reg[fbzMode].u) == 0);
	}
	if (FBZMODE_AUX_BUFFER_MASK(v->reg[fbzMode].u) && v->fbi.auxoffs != (UINT32)(~0)) {
		clear_mask |= GL_DEPTH_BUFFER_BIT;
		mgl_set_depth_mask(true);
		glClearDepth((float)((UINT16)v->reg[zaColor].u)/65535.0f);
	}

	if (clear_mask) glClear(clear_mask);

	if (scissors_needed) {
		glScissor(0, 0, (int)new_width, (int)new_height);
		glDisable(GL_SCISSOR_TEST);
	}
}

void voodoo_ogl_clear(void) {
	mgl_end();
	mgl_set_draw_front(false);
	mgl_set_color_mask(true);
	mgl_set_depth_mask(true);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	last_clear_color = 0;
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	voodoo_ogl_swap_buffer();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void voodoo_ogl_swap_buffer(void) {
	mgl_end();
	mglSwitchDisplay();
	front_dirty = false;
	cached_line_y[0] = cached_line_y[1] = -1;
}

void voodoo_ogl_vblank_flush(void) {
	mgl_end();
	if (front_dirty && !mgl_fullscreen) {
		/* windowed: one surface, present it so front-buffer drawing shows */
		mglSwitchDisplay();
		front_dirty = false;
	} else {
		glFlush();
	}
}

/* Amiga raw key codes -> DOSBox keys (US/PC-style layout). */
static KBD_KEYS mgl_rawkey_map(int code) {
	static const KBD_KEYS map[0x72] = {
		/* 00 */ KBD_grave, KBD_1, KBD_2, KBD_3, KBD_4, KBD_5, KBD_6, KBD_7,
		/* 08 */ KBD_8, KBD_9, KBD_0, KBD_minus, KBD_equals, KBD_backslash, KBD_NONE, KBD_kp0,
		/* 10 */ KBD_q, KBD_w, KBD_e, KBD_r, KBD_t, KBD_y, KBD_u, KBD_i,
		/* 18 */ KBD_o, KBD_p, KBD_leftbracket, KBD_rightbracket, KBD_NONE, KBD_kp1, KBD_kp2, KBD_kp3,
		/* 20 */ KBD_a, KBD_s, KBD_d, KBD_f, KBD_g, KBD_h, KBD_j, KBD_k,
		/* 28 */ KBD_l, KBD_semicolon, KBD_quote, KBD_backslash, KBD_NONE, KBD_kp4, KBD_kp5, KBD_kp6,
		/* 30 */ KBD_extra_lt_gt, KBD_z, KBD_x, KBD_c, KBD_v, KBD_b, KBD_n, KBD_m,
		/* 38 */ KBD_comma, KBD_period, KBD_slash, KBD_NONE, KBD_kpperiod, KBD_kp7, KBD_kp8, KBD_kp9,
		/* 40 */ KBD_space, KBD_backspace, KBD_tab, KBD_kpenter, KBD_enter, KBD_esc, KBD_delete, KBD_insert,
		/* 48 */ KBD_pageup, KBD_pagedown, KBD_kpminus, KBD_f11, KBD_up, KBD_down, KBD_right, KBD_left,
		/* 50 */ KBD_f1, KBD_f2, KBD_f3, KBD_f4, KBD_f5, KBD_f6, KBD_f7, KBD_f8,
		/* 58 */ KBD_f9, KBD_f10, KBD_numlock, KBD_scrolllock, KBD_kpdivide, KBD_kpmultiply, KBD_kpplus, KBD_f12,
		/* 60 */ KBD_leftshift, KBD_rightshift, KBD_capslock, KBD_leftctrl, KBD_leftalt, KBD_rightalt, KBD_NONE, KBD_rightctrl,
		/* 68 */ KBD_NONE, KBD_NONE, KBD_NONE, KBD_NONE, KBD_NONE, KBD_NONE, KBD_pause, KBD_f12,
		/* 70 */ KBD_home, KBD_end
	};
	return (code >= 0 && code < 0x72) ? map[code] : KBD_NONE;
}

static void mgl_poll_input(void) {
	static bool ctrl_down = false;
	WOS_MGLEvent ev;
	while (WOS_MGLInput_Next(&ev)) {
		switch (ev.type) {
		case WOS_MGL_EVENT_KEY: {
			const KBD_KEYS key = mgl_rawkey_map(ev.code);
			if (key == KBD_leftctrl || key == KBD_rightctrl) ctrl_down = ev.down;
			/* Ctrl+F9 quits, as with the SDL mapper's default binding */
			if (key == KBD_f9 && ev.down && ctrl_down) DOSBOX_Quit();
			if (key != KBD_NONE) KEYBOARD_AddKey(key, ev.down != 0);
			break;
		}
		case WOS_MGL_EVENT_MOUSEMOVE:
			Mouse_CursorMoved((float)ev.dx, (float)ev.dy, 0.0f, 0.0f, true);
			break;
		case WOS_MGL_EVENT_BUTTON:
			if (ev.down) Mouse_ButtonPressed((Bit8u)ev.code);
			else Mouse_ButtonReleased((Bit8u)ev.code);
			break;
		}
	}
}

/* Called from the Voodoo vertical timer every frame in accelerated mode. */
void voodoo_ogl_set_window(voodoo_state *v) {
	mgl_end();
	mgl_poll_input();

	bool size_changed = (new_width != last_width) || (new_height != last_height);
	if (size_changed || (last_orientation != (INT32)FBZMODE_Y_ORIGIN(v->reg[fbzMode].u))) {
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		if (FBZMODE_Y_ORIGIN(v->reg[fbzMode].u))
			glOrtho(0, new_width, 0, new_height, 0.0f, -1.0f);
		else
			glOrtho(0, new_width, new_height, 0, 0.0f, -1.0f);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		last_orientation = (INT32)FBZMODE_Y_ORIGIN(v->reg[fbzMode].u);
	}
	if (size_changed) {
		glViewport(0, 0, (int)new_width, (int)new_height);
		last_width = new_width;
		last_height = new_height;
	}
}

/*************************************
 *  Context lifecycle
 *************************************/

static bool mgl_create_context(UINT32 w, UINT32 h) {
	mglChooseWindowMode(mgl_fullscreen ? GL_FALSE : GL_TRUE);
	mglChoosePixelDepth(mgl_depth);
	mglChooseNumberOfBuffers(2);
	mglChooseVertexBufferSize(4096);
	if (!mglCreateContext(0, 0, (int)w, (int)h)) return false;
	new_width = w;
	new_height = h;
	return true;
}

static void mgl_close_context(void) {
	if (!mgl_ctx) return;
	mgl_end();
	mgl_flush_all_textures();
	WOS_MGLInput_Detach();
	mglDeleteContext();
	mgl_ctx = false;
	WOS_Video_ToFront();	/* back to DOSBox's own screen and its input */
	for (int i = 0; i < 2; i++) {
		free(cached_line[i]);
		cached_line[i] = NULL;
		cached_line_y[i] = -1;
	}
	cached_line_len = 0;
}

bool voodoo_ogl_init(voodoo_state *v) {
	if (mgl_ctx) return true;
	if (mgl_lib_failed) return false;

	if (!mgl_lib_open) {
		Section_prop *section = static_cast<Section_prop *>(control->GetSection("voodoo"));
		if (section) {
			mgl_fullscreen = section->Get_bool("voodoo_minigl_fullscreen");
			mgl_depth = section->Get_int("voodoo_minigl_depth");
		}
		if (!MiniGLOpen()) {
			LOG_MSG("VOODOO: MiniGLOpen failed - is minigl_ppc.dll next to DOSBox and the MiniGL host running? Using software rendering.");
			mgl_lib_failed = true;
			return false;
		}
		mgl_lib_open = true;
		mgl_init_crc();
	}

	/* the Voodoo mode itself, else 640x480 (or 800x600) with scaling */
	UINT32 w = v->fbi.width, h = v->fbi.height;
	if (!mgl_create_context(w, h)) {
		w = (v->fbi.width > 640 || v->fbi.height > 480) ? 800 : 640;
		h = (w == 800) ? 600 : 480;
		if (!mgl_create_context(w, h)) {
			LOG_MSG("VOODOO: mglCreateContext(%dx%d) failed, using software rendering", (int)w, (int)h);
			return false;
		}
	}
	mgl_ctx = true;
	LOG_MSG("VOODOO: MiniGL %s %dx%dx%d for a %dx%d Voodoo display",
		mgl_fullscreen ? "screen" : "window", (int)new_width, (int)new_height, mgl_depth,
		(int)v->fbi.width, (int)v->fbi.height);

	mgl_reset_state_cache();
	texture_dirty[0].pending = texture_dirty[1].pending = false;
	last_width = last_height = 0;
	last_orientation = -1;
	last_clear_color = 0xffffffff;
	front_dirty = false;

	glShadeModel(GL_SMOOTH);
	glDisable(GL_CULL_FACE);
	glDisable(GL_FOG);
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glDepthRange(0.0, 1.0);
	voodoo_ogl_set_window(v);

	if (!WOS_MGLInput_Attach(mglGetWindowHandle()))
		LOG_MSG("VOODOO: no input from the MiniGL window");
	return true;
}

void voodoo_ogl_leave(bool leavemode) {
	mgl_close_context();
}

void voodoo_ogl_update_dimensions(void) {
	if (!mgl_ctx || !v) return;
	if (v->fbi.width == new_width && v->fbi.height == new_height) return;
	/* reopen at the new Voodoo resolution */
	mgl_close_context();
	if (!voodoo_ogl_init(v)) {
		LOG_MSG("VOODOO: MiniGL context lost on resolution change");
		v->ogl = false;
	} else {
		voodoo_ogl_clear();
	}
}

void voodoo_ogl_shutdown(voodoo_state *v) {
	mgl_close_context();
	if (mgl_lib_open) {
		MiniGLClose();
		mgl_lib_open = false;
	}
}

#else

/* no accelerated backend: the core falls back to software */
bool voodoo_ogl_init(voodoo_state *v) { return false; }
void voodoo_ogl_leave(bool leavemode) { }
void voodoo_ogl_shutdown(voodoo_state *v) { }
void voodoo_ogl_set_window(voodoo_state *v) { }
void voodoo_ogl_swap_buffer(void) { }
void voodoo_ogl_vblank_flush(void) { }
void voodoo_ogl_update_dimensions(void) { }
void voodoo_ogl_clear(void) { }
void voodoo_ogl_fastfill(void) { }
void voodoo_ogl_clip_window(voodoo_state *v) { }
void voodoo_ogl_texture_clear(UINT32 address, int TMU) { }
void voodoo_ogl_invalidate_paltex(void) { }
void voodoo_ogl_draw_pixel(int x, int y, bool has_rgb, bool has_alpha, int r, int g, int b, int a) { }
void voodoo_ogl_draw_z(int x, int y, int z) { }
void voodoo_ogl_draw_pixel_pipeline(int x, int y, int r, int g, int b) { }
UINT32 voodoo_ogl_read_pixel(int x, int y) { return 0xffffffff; }
void voodoo_ogl_draw_triangle(poly_extra_data *extra) { }

#endif

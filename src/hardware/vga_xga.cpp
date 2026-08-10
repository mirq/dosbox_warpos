/*
 *  Copyright (C) 2002-2020  The DOSBox Team
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


#include <string.h>
#include "dosbox.h"
#include "inout.h"
#include "vga.h"
#include <math.h>
#include <stdio.h>
#include "callback.h"
#include "cpu.h"		// for 0x3da delay
#ifdef __MORPHOS__
#include "morphos_altivec.h"
#include "morphos_runtime.h"
#endif

#define XGA_SCREEN_WIDTH	vga.s3.xga_screen_width
#define XGA_COLOR_MODE		vga.s3.xga_color_mode

#define XGA_SHOW_COMMAND_TRACE 0

struct XGAStatus {
	struct scissorreg {
		Bit16u x1, y1, x2, y2;
	} scissors;

	Bit32u readmask;
	Bit32u writemask;

	Bit32u forecolor;
	Bit32u backcolor;

	Bit32u color_compare;

	Bitu curcommand;

	Bit16u foremix;
	Bit16u backmix;

	Bit16u curx, cury;
	Bit16u destx, desty;

	Bit16u ErrTerm;
	Bit16u MIPcount;
	Bit16u MAPcount;

	Bit16u pix_cntl;
	Bit16u control1;
	Bit16u control2;
	Bit16u read_sel;

	struct XGA_WaitCmd {
		bool newline;
		bool wait;
		Bit16u cmd;
		Bit16u curx, cury;
		Bit16u x1, y1, x2, y2, sizex, sizey;
		Bit32u data; /* transient data passed by multiple calls */
		Bitu datasize;
		Bitu buswidth;
	} waitcmd;

};

static XGAStatus xga;

static void XGA_Write_Multifunc(Bitu val, Bitu len) {
	(void)len;
	Bitu regselect = val >> 12;
	Bitu dataval = val & 0xfff;
	switch(regselect) {
		case 0: // minor axis pixel count
			xga.MIPcount = dataval;
			break;
		case 1: // top scissors
			xga.scissors.y1 = dataval;
			break;
		case 2: // left
			xga.scissors.x1 = dataval;
			break;
		case 3: // bottom
			xga.scissors.y2 = dataval;
			break;
		case 4: // right
			xga.scissors.x2 = dataval;
			break;
		case 0xa: // data manip control
			xga.pix_cntl = dataval;
			break;
		case 0xd: // misc 2
			xga.control2 = dataval;
			break;
		case 0xe:
			xga.control1 = dataval;
			break;
		case 0xf:
			xga.read_sel = dataval;
			break;
		default:
			LOG_MSG("XGA: Unhandled multifunction command %x", regselect);
			break;
	}
}

static Bitu XGA_Read_Multifunc() {
	switch(xga.read_sel++) {
		case 0: return xga.MIPcount;
		case 1: return xga.scissors.y1;
		case 2: return xga.scissors.x1;
		case 3: return xga.scissors.y2;
		case 4: return xga.scissors.x2;
		case 5: return xga.pix_cntl;
		case 6: return xga.control1;
		case 7: return 0; // TODO
		case 8: return 0; // TODO
		case 9: return 0; // TODO
		case 10: return xga.control2;
		default: return 0;
	}
}


static void XGA_DrawPoint(Bitu x, Bitu y, Bitu c) {
	if((xga.curcommand & 0x11) != 0x11) return;

	if(x < xga.scissors.x1) return;
	if(x > xga.scissors.x2) return;
	if(y < xga.scissors.y1) return;
	if(y > xga.scissors.y2) return;

	Bit32u memaddr = (y * XGA_SCREEN_WIDTH) + x;
	/* Need to zero out all unused bits in modes that have any (15-bit or "32"-bit -- the last
	   one is actually 24-bit. Without this step there may be some graphics corruption (mainly,
	   during windows dragging. */
	switch(XGA_COLOR_MODE) {
		case M_LIN8:
			if (GCC_UNLIKELY(memaddr >= vga.vmemsize)) break;
			vga.mem.linear[memaddr] = c;
			break;
		case M_LIN15:
			if (GCC_UNLIKELY(memaddr*2 >= vga.vmemsize)) break;
			((Bit16u*)(vga.mem.linear))[memaddr] = (Bit16u)(c&0x7fff);
			break;
		case M_LIN16:
			if (GCC_UNLIKELY(memaddr*2 >= vga.vmemsize)) break;
			((Bit16u*)(vga.mem.linear))[memaddr] = (Bit16u)(c&0xffff);
			break;
		case M_LIN32:
			if (GCC_UNLIKELY(memaddr*4 >= vga.vmemsize)) break;
			((Bit32u*)(vga.mem.linear))[memaddr] = c;
			break;
		default:
			break;
	}

}

static Bitu XGA_GetPoint(Bitu x, Bitu y) {
	Bit32u memaddr = (y * XGA_SCREEN_WIDTH) + x;

	switch(XGA_COLOR_MODE) {
	case M_LIN8:
		if (GCC_UNLIKELY(memaddr >= vga.vmemsize)) break;
		return vga.mem.linear[memaddr];
	case M_LIN15:
	case M_LIN16:
		if (GCC_UNLIKELY(memaddr*2 >= vga.vmemsize)) break;
		return ((Bit16u*)(vga.mem.linear))[memaddr];
	case M_LIN32:
		if (GCC_UNLIKELY(memaddr*4 >= vga.vmemsize)) break;
		return ((Bit32u*)(vga.mem.linear))[memaddr];
	default:
		break;
	}
	return 0;
}


/* Resolve the raster operation and fetch the destination pixel only for
 * operations that actually use it. This preserves the exact ROP result while
 * avoiding a VRAM read for FALSE, TRUE, NOT SRC and SRC. */
static Bitu XGA_GetMixResult(Bitu mixmode, Bitu srcval, Bitu x, Bitu y) {
	switch(mixmode & 0xf) {
		case 0x00: /* not DST */
			return ~XGA_GetPoint(x, y);
		case 0x01: /* 0 (false) */
			return 0;
		case 0x02: /* 1 (true) */
			return 0xffffffff;
		case 0x03: /* DST */
			return XGA_GetPoint(x, y);
		case 0x04: /* not SRC */
			return ~srcval;
		case 0x05: /* SRC xor DST */
			return srcval ^ XGA_GetPoint(x, y);
		case 0x06: /* not (SRC xor DST) */
			return ~(srcval ^ XGA_GetPoint(x, y));
		case 0x07: /* SRC */
			return srcval;
		case 0x08: /* not (SRC and DST) */
			return ~(srcval & XGA_GetPoint(x, y));
		case 0x09: /* (not SRC) or DST */
			return (~srcval) | XGA_GetPoint(x, y);
		case 0x0a: /* SRC or (not DST) */
			return srcval | (~XGA_GetPoint(x, y));
		case 0x0b: /* SRC or DST */
			return srcval | XGA_GetPoint(x, y);
		case 0x0c: /* SRC and DST */
			return srcval & XGA_GetPoint(x, y);
		case 0x0d: /* SRC and (not DST) */
			return srcval & (~XGA_GetPoint(x, y));
		case 0x0e: /* (not SRC) and DST */
			return (~srcval) & XGA_GetPoint(x, y);
		case 0x0f: /* not (SRC or DST) */
			return ~(srcval | XGA_GetPoint(x, y));
		default:
			LOG_MSG("XGA: GetMixResult: Unknown mix.  Shouldn't be able to get here!");
			return 0;
	}
}

static inline void XGA_DrawMixedPoint(Bitu x, Bitu y, Bitu mixmode, Bitu srcval) {
	XGA_DrawPoint(x, y, XGA_GetMixResult(mixmode, srcval, x, y));
}

static inline bool XGA_ColorCompareAllows(Bitu srcval, Bitu colorcmpdata,
                                          bool enabled, bool src_ne) {
	return !enabled || ((srcval != colorcmpdata) != src_ne);
}

#ifdef __MORPHOS__
static bool XGA_HasUsableAltiVec(void) {
	static int tested = 0;
	static bool usable = false;
	if(!tested) {
		usable = MORPHOS_HasAltiVec() && MORPHOS_AltivecSelfTest();
		tested = 1;
	}
	return usable;
}
#endif


/* Return true only when an 8-bit destination rectangle can be written as
 * complete rows without changing clipping or out-of-range behavior. The
 * normal pixel path remains responsible for every irregular case. */
static bool XGA_GetFastDestRect8(Bits startx, Bits starty, Bits dx, Bits dy,
                                 Bit32u width, Bit32u height,
                                 Bit32u &left, Bit32u &top) {
	if(XGA_COLOR_MODE != M_LIN8 || (xga.curcommand & 0x11) != 0x11 ||
	   XGA_SCREEN_WIDTH == 0 || width == 0 || height == 0)
		return false;

	const Bits xlast = startx + dx * (Bits)(width - 1);
	const Bits ylast = starty + dy * (Bits)(height - 1);
	const Bits xlo = (startx < xlast) ? startx : xlast;
	const Bits xhi = (startx > xlast) ? startx : xlast;
	const Bits ylo = (starty < ylast) ? starty : ylast;
	const Bits yhi = (starty > ylast) ? starty : ylast;

	if(xlo < 0 || ylo < 0 || xhi < xlo || yhi < ylo)
		return false;
	if((Bitu)xhi >= XGA_SCREEN_WIDTH)
		return false;
	if((Bitu)xlo < xga.scissors.x1 || (Bitu)xhi > xga.scissors.x2 ||
	   (Bitu)ylo < xga.scissors.y1 || (Bitu)yhi > xga.scissors.y2)
		return false;

	const Bit32u first = (Bit32u)ylo * (Bit32u)XGA_SCREEN_WIDTH + (Bit32u)xlo;
	const Bit32u last = (Bit32u)yhi * (Bit32u)XGA_SCREEN_WIDTH + (Bit32u)xhi;
	if(first >= vga.vmemsize || last >= vga.vmemsize)
		return false;

	left = (Bit32u)xlo;
	top = (Bit32u)ylo;
	return true;
}

static bool XGA_TryFastSolidFill8(Bits startx, Bits starty, Bits dx, Bits dy,
                                  Bit32u width, Bit32u height, Bitu srcval) {
	Bit32u left, top;
	if(!XGA_GetFastDestRect8(startx, starty, dx, dy,
	                         width, height, left, top))
		return false;

	Bit8u *row = vga.mem.linear + top * (Bit32u)XGA_SCREEN_WIDTH + left;
	for(Bit32u y = 0; y < height; ++y) {
		memset(row, (int)(srcval & 0xffu), width);
		row += XGA_SCREEN_WIDTH;
	}
	return true;
}

/* Fast VRAM-to-VRAM copy used by the common 8-bit ROP=SRC command. It is
 * deliberately limited to fully visible, in-range and non-overlapping
 * rectangles. Overlapping or unusual commands retain the original ordered
 * pixel loop, so guest-selected copy direction keeps its exact behavior. */
static bool XGA_TryFastBitmapCopy8(Bits dx, Bits dy, Bit32u colorcmpdata,
                                   bool color_cmp_enabled,
                                   bool color_cmp_src_ne) {
	const Bit32u width = (Bit32u)xga.MAPcount + 1u;
	const Bit32u height = (Bit32u)xga.MIPcount + 1u;
	Bit32u src_left, src_top, dst_left, dst_top;

	if(!XGA_GetFastDestRect8(xga.destx, xga.desty, dx, dy,
	                         width, height, dst_left, dst_top))
		return false;

	/* Source pixels are not clipped, but they must be valid contiguous rows. */
	if(XGA_COLOR_MODE != M_LIN8 || XGA_SCREEN_WIDTH == 0)
		return false;
	const Bits src_last_x = (Bits)xga.curx + dx * (Bits)(width - 1u);
	const Bits src_last_y = (Bits)xga.cury + dy * (Bits)(height - 1u);
	const Bits src_xlo = ((Bits)xga.curx < src_last_x) ? (Bits)xga.curx : src_last_x;
	const Bits src_xhi = ((Bits)xga.curx > src_last_x) ? (Bits)xga.curx : src_last_x;
	const Bits src_ylo = ((Bits)xga.cury < src_last_y) ? (Bits)xga.cury : src_last_y;
	const Bits src_yhi = ((Bits)xga.cury > src_last_y) ? (Bits)xga.cury : src_last_y;
	if(src_xlo < 0 || src_ylo < 0 || src_xhi < src_xlo || src_yhi < src_ylo ||
	   (Bitu)src_xhi >= XGA_SCREEN_WIDTH)
		return false;

	src_left = (Bit32u)src_xlo;
	src_top = (Bit32u)src_ylo;
	const Bit32u src_first = src_top * (Bit32u)XGA_SCREEN_WIDTH + src_left;
	const Bit32u src_last = (Bit32u)src_yhi * (Bit32u)XGA_SCREEN_WIDTH +
	                       (Bit32u)src_xhi;
	const Bit32u dst_first = dst_top * (Bit32u)XGA_SCREEN_WIDTH + dst_left;
	const Bit32u dst_last = (dst_top + height - 1u) * (Bit32u)XGA_SCREEN_WIDTH +
	                       (dst_left + width - 1u);
	if(src_first >= vga.vmemsize || src_last >= vga.vmemsize)
		return false;

	/* Conservative whole-rectangle overlap check. False positives only select
	 * the old path; no overlapping command reaches memcpy. */
	if(!(src_last < dst_first || dst_last < src_first))
		return false;

	const Bit8u cmp = (Bit8u)colorcmpdata;
	const Bit8u *first_src = vga.mem.linear +
	                         src_top * (Bit32u)XGA_SCREEN_WIDTH + src_left;
	Bit8u *first_dst = vga.mem.linear +
	                   dst_top * (Bit32u)XGA_SCREEN_WIDTH + dst_left;

#ifdef __MORPHOS__
	/* A color-keyed row is a compare/select operation, which maps directly to
	 * AltiVec. Keep memcpy for opaque copies and use VMX only when the row is
	 * wide enough to amortize the call and both pointers can be aligned by the
	 * same scalar prefix. Every other case retains the scalar implementation. */
	if(color_cmp_enabled && width >= 64u && XGA_HasUsableAltiVec() &&
	   ((((IPTR)first_src ^ (IPTR)first_dst) & 15u) == 0u)) {
		MORPHOS_AltivecColorKeyCopy8Rows(first_src, first_dst,
		                                     width, height,
		                                     (ULONG)XGA_SCREEN_WIDTH,
		                                     cmp,
		                                     color_cmp_src_ne ? TRUE : FALSE);
		return true;
	}
#endif

	for(Bit32u y = 0; y < height; ++y) {
		const Bit8u *src = first_src + y * (Bit32u)XGA_SCREEN_WIDTH;
		Bit8u *dst = first_dst + y * (Bit32u)XGA_SCREEN_WIDTH;
		if(!color_cmp_enabled) {
			memcpy(dst, src, width);
		} else if(!color_cmp_src_ne) {
			/* SRC_NE clear: equal pixels are transparent. */
			for(Bit32u x = 0; x < width; ++x)
				if(src[x] != cmp) dst[x] = src[x];
		} else {
			/* SRC_NE set: non-equal pixels are transparent. */
			for(Bit32u x = 0; x < width; ++x)
				if(src[x] == cmp) dst[x] = src[x];
		}
	}
	return true;
}

/* Fast path for the common rectangle-fill form where FOREMIX and its solid
 * source color remain constant for the entire synchronous command. */
static void XGA_DrawSolidRectangle(Bits dx, Bits dy, Bitu mixmode, Bitu srcval) {
	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;
	Bits srcy = xga.cury;
	Bits srcx = xga.curx;

	/* ROP DST leaves every destination pixel unchanged. Rectangle commands
	 * still update CUR_X/CUR_Y, so reproduce only that final state. */
	if((mixmode & 0x0f) == 0x03) {
		xga.curx = (Bit16u)(srcx + dx * (Bits)(width + 1));
		xga.cury = (Bit16u)(srcy + dy * (Bits)(height + 1));
		return;
	}

	/* ROP SRC is a direct solid fill and does not need the ROP mixer. */
	if((mixmode & 0x0f) == 0x07) {
		if(!XGA_TryFastSolidFill8(xga.curx, xga.cury, dx, dy,
		                             width + 1u, height + 1u, srcval)) {
			for(Bit32u yat = 0; yat <= height; ++yat) {
				srcx = xga.curx;
				for(Bit32u xat = 0; xat <= width; ++xat) {
					XGA_DrawPoint(srcx, srcy, srcval);
					srcx += dx;
				}
				srcy += dy;
			}
		} else {
			srcx = (Bits)xga.curx + dx * (Bits)(width + 1u);
			srcy = (Bits)xga.cury + dy * (Bits)(height + 1u);
		}
		xga.curx = (Bit16u)srcx;
		xga.cury = (Bit16u)srcy;
		return;
	}

	for(Bit32u yat = 0; yat <= height; ++yat) {
		srcx = xga.curx;
		for(Bit32u xat = 0; xat <= width; ++xat) {
			XGA_DrawMixedPoint(srcx, srcy, mixmode, srcval);
			srcx += dx;
		}
		srcy += dy;
	}

	xga.curx = (Bit16u)srcx;
	xga.cury = (Bit16u)srcy;
}

static inline void XGA_BlitWritePixel(Bitu tarx, Bitu tary, Bitu mixmode,
                                      Bitu srcval, Bitu colorcmpdata,
                                      bool color_cmp_enabled,
                                      bool color_cmp_src_ne) {
	if(XGA_ColorCompareAllows(srcval, colorcmpdata,
	                          color_cmp_enabled, color_cmp_src_ne))
		XGA_DrawMixedPoint(tarx, tary, mixmode, srcval);
}

static void XGA_BlitRectConstantSource(Bits dx, Bits dy, Bitu mixmode,
                                       Bitu srcval, Bitu colorcmpdata,
                                       bool color_cmp_enabled,
                                       bool color_cmp_src_ne) {
	/* ROP DST is a complete no-op for BitBLT. A constant source also has one
	 * COLOR_CMP result for the whole command, so evaluate it once. */
	if((mixmode & 0x0f) == 0x03 ||
	   !XGA_ColorCompareAllows(srcval, colorcmpdata,
	                           color_cmp_enabled, color_cmp_src_ne))
		return;

	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;
	Bits tary = xga.desty;

	if((mixmode & 0x0f) == 0x07) {
		if(XGA_TryFastSolidFill8(xga.destx, xga.desty, dx, dy,
		                            width + 1u, height + 1u, srcval))
			return;
		for(Bit32u yat = 0; yat <= height; ++yat) {
			Bits tarx = xga.destx;
			for(Bit32u xat = 0; xat <= width; ++xat) {
				XGA_DrawPoint(tarx, tary, srcval);
				tarx += dx;
			}
			tary += dy;
		}
		return;
	}

	for(Bit32u yat = 0; yat <= height; ++yat) {
		Bits tarx = xga.destx;
		for(Bit32u xat = 0; xat <= width; ++xat) {
			XGA_DrawMixedPoint(tarx, tary, mixmode, srcval);
			tarx += dx;
		}
		tary += dy;
	}
}

static void XGA_BlitRectBitmapSource(Bits dx, Bits dy, Bitu mixmode,
                                     Bitu colorcmpdata,
                                     bool color_cmp_enabled,
                                     bool color_cmp_src_ne) {
	/* ROP DST neither reads nor changes externally visible S3 state. */
	if((mixmode & 0x0f) == 0x03)
		return;

	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;
	Bits srcy = xga.cury;
	Bits tary = xga.desty;

	if((mixmode & 0x0f) == 0x07) {
		if(XGA_TryFastBitmapCopy8(dx, dy, colorcmpdata,
		                              color_cmp_enabled, color_cmp_src_ne))
			return;
		for(Bit32u yat = 0; yat <= height; ++yat) {
			Bits srcx = xga.curx;
			Bits tarx = xga.destx;
			for(Bit32u xat = 0; xat <= width; ++xat) {
				const Bitu srcval = XGA_GetPoint(srcx, srcy);
				if(XGA_ColorCompareAllows(srcval, colorcmpdata,
				                          color_cmp_enabled, color_cmp_src_ne))
					XGA_DrawPoint(tarx, tary, srcval);
				srcx += dx;
				tarx += dx;
			}
			srcy += dy;
			tary += dy;
		}
		return;
	}

	for(Bit32u yat = 0; yat <= height; ++yat) {
		Bits srcx = xga.curx;
		Bits tarx = xga.destx;
		for(Bit32u xat = 0; xat <= width; ++xat) {
			const Bitu srcval = XGA_GetPoint(srcx, srcy);
			XGA_BlitWritePixel(tarx, tary, mixmode, srcval, colorcmpdata,
			                   color_cmp_enabled, color_cmp_src_ne);
			srcx += dx;
			tarx += dx;
		}
		srcy += dy;
		tary += dy;
	}
}

static void XGA_DrawLineVector(Bitu val) {
	Bits xat, yat;
	Bitu srcval = 0;
	Bits i;

	Bits dx, sx, sy;

	dx = xga.MAPcount; 
	xat = xga.curx;
	yat = xga.cury;

	switch((val >> 5) & 0x7) {
		case 0x00: /* 0 degrees */
			sx = 1;
			sy = 0;
			break;
		case 0x01: /* 45 degrees */
			sx = 1;
			sy = -1;
			break;
		case 0x02: /* 90 degrees */
			sx = 0;
			sy = -1;
			break;
		case 0x03: /* 135 degrees */
			sx = -1;
			sy = -1;
			break;
		case 0x04: /* 180 degrees */
			sx = -1;
			sy = 0;
			break;
		case 0x05: /* 225 degrees */
			sx = -1;
			sy = 1;
			break;
		case 0x06: /* 270 degrees */
			sx = 0;
			sy = 1;
			break;
		case 0x07: /* 315 degrees */
			sx = 1;
			sy = 1;
			break;
		default:  // Should never get here
			sx = 0;
			sy = 0;
			break;
	}

	for (i=0;i<=dx;i++) {
		Bitu mixmode = (xga.pix_cntl >> 6) & 0x3;
		switch (mixmode) {
			case 0x00: /* FOREMIX always used */
				mixmode = xga.foremix;
				switch((mixmode >> 5) & 0x03) {
					case 0x00: /* Src is background color */
						srcval = xga.backcolor;
						break;
					case 0x01: /* Src is foreground color */
						srcval = xga.forecolor;
						break;
					case 0x02: /* Src is pixel data from PIX_TRANS register */
						//srcval = tmpval;
						//LOG_MSG("XGA: DrawRect: Wants data from PIX_TRANS register");
						break;
					case 0x03: /* Src is bitmap data */
						LOG_MSG("XGA: DrawRect: Wants data from srcdata");
						//srcval = srcdata;
						break;
					default:
						LOG_MSG("XGA: DrawRect: Shouldn't be able to get here!");
						break;
				}
				XGA_DrawMixedPoint(xat, yat, mixmode, srcval);
				break;
			default: 
				LOG_MSG("XGA: DrawLine: Needs mixmode %x", mixmode);
				break;
		}
		xat += sx;
		yat += sy;
	}

	xga.curx = xat-1;
	xga.cury = yat;
}

static void XGA_DrawLineBresenham(Bitu val) {
	Bits xat, yat;
	Bitu srcval = 0;
	Bitu destval;
	Bits i;
	Bits tmpswap;
	bool steep;

#define SWAP(a,b) tmpswap = a; a = b; b = tmpswap;

	Bits dx, sx, dy, sy, e, dmajor, dminor,destxtmp;

	// Probably a lot easier way to do this, but this works.

	dminor = (Bits)((Bit16s)xga.desty);
	if(xga.desty&0x2000) dminor |= ~0x1fff;
	dminor >>= 1;

	destxtmp=(Bits)((Bit16s)xga.destx);
	if(xga.destx&0x2000) destxtmp |= ~0x1fff;


	dmajor = -(destxtmp - (dminor << 1)) >> 1;
	
	dx = dmajor;
	if((val >> 5) & 0x1) {
        sx = 1;
	} else {
		sx = -1;
	}
	dy = dminor;
	if((val >> 7) & 0x1) {
        sy = 1;
	} else {
		sy = -1;
	}
	e = (Bits)((Bit16s)xga.ErrTerm);
	if(xga.ErrTerm&0x2000) e |= ~0x1fff;
	xat = xga.curx;
	yat = xga.cury;

	if((val >> 6) & 0x1) {
		steep = false;
		SWAP(xat, yat);
		SWAP(sx, sy);
	} else {
		steep = true;
	}
    
	//LOG_MSG("XGA: Bresenham: ASC %d, LPDSC %d, sx %d, sy %d, err %d, steep %d, length %d, dmajor %d, dminor %d, xstart %d, ystart %d", dx, dy, sx, sy, e, steep, xga.MAPcount, dmajor, dminor,xat,yat);

	const Bits pixel_count = xga.MAPcount;
	for (i=0;i<=pixel_count;i++) { 
			Bitu mixmode = (xga.pix_cntl >> 6) & 0x3;
			switch (mixmode) {
				case 0x00: /* FOREMIX always used */
					mixmode = xga.foremix;
					switch((mixmode >> 5) & 0x03) {
						case 0x00: /* Src is background color */
							srcval = xga.backcolor;
							break;
						case 0x01: /* Src is foreground color */
							srcval = xga.forecolor;
							break;
						case 0x02: /* Src is pixel data from PIX_TRANS register */
							//srcval = tmpval;
							LOG_MSG("XGA: DrawRect: Wants data from PIX_TRANS register");
							break;
						case 0x03: /* Src is bitmap data */
							LOG_MSG("XGA: DrawRect: Wants data from srcdata");
							//srcval = srcdata;
							break;
						default:
							LOG_MSG("XGA: DrawRect: Shouldn't be able to get here!");
							break;
					}

					if(steep) {
						destval = XGA_GetMixResult(mixmode, srcval, xat, yat);
						XGA_DrawPoint(xat,yat, destval);
					} else {
						destval = XGA_GetMixResult(mixmode, srcval, yat, xat);
						XGA_DrawPoint(yat,xat, destval);
					}

					break;
				default: 
					LOG_MSG("XGA: DrawLine: Needs mixmode %x", mixmode);
					break;
			}
			while (e > 0) {
				yat += sy;
				e -= (dx << 1);
			}
			xat += sx;
			e += (dy << 1);
	}

	if(steep) {
		xga.curx = xat;
		xga.cury = yat;
	} else {
		xga.curx = yat;
		xga.cury = xat;
	}
	//	}
	//}
	
}

static void XGA_DrawRectangle(Bitu val) {
	Bit32u xat, yat;
	Bitu srcval = 0;

	Bits srcx, srcy, dx, dy;

	dx = -1;
	dy = -1;

	if(((val >> 5) & 0x01) != 0) dx = 1;
	if(((val >> 7) & 0x01) != 0) dy = 1;

	/* The supported rectangle-fill path uses a single, fixed FOREMIX. Decode
	 * its solid source once instead of once per destination pixel. */
	if(((xga.pix_cntl >> 6) & 0x3) == 0) {
		const Bitu fixed_mixmode = xga.foremix;
		switch((fixed_mixmode >> 5) & 0x03) {
			case 0x00:
				XGA_DrawSolidRectangle(dx, dy, fixed_mixmode, xga.backcolor);
				return;
			case 0x01:
				XGA_DrawSolidRectangle(dx, dy, fixed_mixmode, xga.forecolor);
				return;
			default:
				break; /* preserve the original diagnostic/fallback path */
		}
	}

	srcy = xga.cury;
	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;

	for(yat=0;yat<=height;yat++) {
		srcx = xga.curx;
		for(xat=0;xat<=width;xat++) {
			Bitu mixmode = (xga.pix_cntl >> 6) & 0x3;
			switch (mixmode) {
				case 0x00: /* FOREMIX always used */
					mixmode = xga.foremix;
					switch((mixmode >> 5) & 0x03) {
						case 0x00: /* Src is background color */
							srcval = xga.backcolor;
							break;
						case 0x01: /* Src is foreground color */
							srcval = xga.forecolor;
							break;
						case 0x02: /* Src is pixel data from PIX_TRANS register */
							//srcval = tmpval;
							LOG_MSG("XGA: DrawRect: Wants data from PIX_TRANS register");
							break;
						case 0x03: /* Src is bitmap data */
							LOG_MSG("XGA: DrawRect: Wants data from srcdata");
							//srcval = srcdata;
							break;
						default:
							LOG_MSG("XGA: DrawRect: Shouldn't be able to get here!");
							break;
					}
					XGA_DrawMixedPoint(srcx, srcy, mixmode, srcval);
					break;
				default: 
					LOG_MSG("XGA: DrawRect: Needs mixmode %x", mixmode);
					break;
			}
			srcx += dx;
		}
		srcy += dy;
	}
	xga.curx = srcx;
	xga.cury = srcy;

	//LOG_MSG("XGA: Draw rect (%d, %d)-(%d, %d), %d", x1, y1, x2, y2, xga.forecolor);
}

static bool XGA_CheckX(void) {
	bool newline = false;
	if(!xga.waitcmd.newline) {
	
	if((xga.waitcmd.curx<2048) && xga.waitcmd.curx > (xga.waitcmd.x2)) {
		xga.waitcmd.curx = xga.waitcmd.x1;
		xga.waitcmd.cury++;
		xga.waitcmd.cury&=0x0fff;
		newline = true;
		xga.waitcmd.newline = true;
		if((xga.waitcmd.cury<2048)&&(xga.waitcmd.cury > xga.waitcmd.y2))
			xga.waitcmd.wait = false;
	} else if(xga.waitcmd.curx>=2048) {
		Bit16u realx = 4096-xga.waitcmd.curx;
		if(xga.waitcmd.x2>2047) { // x end is negative too
			Bit16u realxend=4096-xga.waitcmd.x2;
			if(realx==realxend) {
				xga.waitcmd.curx = xga.waitcmd.x1;
				xga.waitcmd.cury++;
				xga.waitcmd.cury&=0x0fff;
				newline = true;
				xga.waitcmd.newline = true;
				if((xga.waitcmd.cury<2048)&&(xga.waitcmd.cury > xga.waitcmd.y2))
					xga.waitcmd.wait = false;
			}
		} else { // else overlapping
			if(realx==xga.waitcmd.x2) {
				xga.waitcmd.curx = xga.waitcmd.x1;
				xga.waitcmd.cury++;
				xga.waitcmd.cury&=0x0fff;
				newline = true;
				xga.waitcmd.newline = true;
				if((xga.waitcmd.cury<2048)&&(xga.waitcmd.cury > xga.waitcmd.y2))
					xga.waitcmd.wait = false;
				}
			}
		}
	} else {
        xga.waitcmd.newline = false;
	}
	return newline;
}

static inline void XGA_AdvanceWaitPixel(void) {
	xga.waitcmd.curx++;
	xga.waitcmd.curx &= 0x0fff;
	XGA_CheckX();
}

/* The PIX_TRANS path is dominated by plain source copies. Handle the two
 * identity ROPs before the generic mixer: SRC does not need a destination
 * read, while DST does not need a VRAM access at all. */
static void XGA_DrawWaitSub(Bitu mixmode, Bitu srcval) {
	const Bitu rop = mixmode & 0x0f;

	if(GCC_LIKELY(rop == 0x07)) { /* SRC */
		if(XGA_COLOR_MODE == M_LIN8) {
			const Bitu x = xga.waitcmd.curx;
			const Bitu y = xga.waitcmd.cury;
			if(GCC_LIKELY((xga.curcommand & 0x11) == 0x11) &&
			   GCC_LIKELY(x >= xga.scissors.x1 && x <= xga.scissors.x2) &&
			   GCC_LIKELY(y >= xga.scissors.y1 && y <= xga.scissors.y2)) {
				const Bit32u memaddr = (y * XGA_SCREEN_WIDTH) + x;
				if(GCC_LIKELY(memaddr < vga.vmemsize))
					vga.mem.linear[memaddr] = (Bit8u)srcval;
			}
		} else {
			XGA_DrawPoint(xga.waitcmd.curx, xga.waitcmd.cury, srcval);
		}
		XGA_AdvanceWaitPixel();
		return;
	}

	if(GCC_UNLIKELY(rop == 0x03) && XGA_COLOR_MODE != M_LIN15) {
		/* In 15-bit mode the legacy read/write pair also clears bit 15. */
		XGA_AdvanceWaitPixel();
		return;
	}

	XGA_DrawMixedPoint(xga.waitcmd.curx, xga.waitcmd.cury, mixmode, srcval);
	XGA_AdvanceWaitPixel();
}

static inline void XGA_DrawWaitFour8(Bitu mixmode, Bitu val) {
	XGA_DrawWaitSub(mixmode,  val        & 0xffu);
	XGA_DrawWaitSub(mixmode, (val >>  8) & 0xffu);
	XGA_DrawWaitSub(mixmode, (val >> 16) & 0xffu);
	XGA_DrawWaitSub(mixmode, (val >> 24) & 0xffu);
}

static void XGA_DrawWait(Bitu val, Bitu len) {
	if(GCC_UNLIKELY(!xga.waitcmd.wait)) return;
	Bitu mixmode = (xga.pix_cntl >> 6) & 0x3;
	Bitu srcval;
	switch(xga.waitcmd.cmd) {
		case 2: /* Rectangle */
			switch(mixmode) {
				case 0x00: /* FOREMIX always used */
					mixmode = xga.foremix;

/*					switch((mixmode >> 5) & 0x03) {
						case 0x00: // Src is background color
							srcval = xga.backcolor;
							break;
						case 0x01: // Src is foreground color
							srcval = xga.forecolor;
							break;
						case 0x02: // Src is pixel data from PIX_TRANS register
*/
					if(((mixmode >> 5) & 0x03) != 0x2) {
						// those cases don't seem to occur
						LOG_MSG("XGA: unsupported drawwait operation");
						break;
					}
					switch(xga.waitcmd.buswidth) {
						case M_LIN8:		//  8 bit
							XGA_DrawWaitSub(mixmode, val);
							break;
						case 0x20 | M_LIN8: // 16 bit 
							for(Bitu i = 0; i < len; i++) {
								XGA_DrawWaitSub(mixmode, (val>>(8*i))&0xff);
								if(xga.waitcmd.newline) break;
							}
							break;
						case 0x40 | M_LIN8: // 32 bit
							XGA_DrawWaitFour8(mixmode, val);
							break;
						case (0x20 | M_LIN32):
							if(len!=4) { // Win 3.11 864 'hack?'
								if(xga.waitcmd.datasize == 0) {
									// set it up to wait for the next word
									xga.waitcmd.data = val; 
									xga.waitcmd.datasize = 2;
									return;
								} else {
									srcval = (val<<16)|xga.waitcmd.data;
									xga.waitcmd.data = 0;
									xga.waitcmd.datasize = 0;
									XGA_DrawWaitSub(mixmode, srcval);
								}
								break;
							} // fall-through
						case 0x40 | M_LIN32: // 32 bit
							XGA_DrawWaitSub(mixmode, val);
							break;
						case 0x20 | M_LIN15: // 16 bit 
						case 0x20 | M_LIN16: // 16 bit 
							XGA_DrawWaitSub(mixmode, val);
							break;
						case 0x40 | M_LIN15: // 32 bit 
						case 0x40 | M_LIN16: // 32 bit 
							XGA_DrawWaitSub(mixmode, val&0xffff);
							if(!xga.waitcmd.newline)
								XGA_DrawWaitSub(mixmode, val>>16);
							break;
						default:
							// Let's hope they never show up ;)
							LOG_MSG("XGA: unsupported bpp / datawidth combination %x",
								xga.waitcmd.buswidth);
							break;
					};
					break;
			
				case 0x02: { // Data from PIX_TRANS selects the mix
					Bitu chunksize = 0;
					Bitu chunks = 0;
					switch(xga.waitcmd.buswidth&0x60) {
						case 0x0:
							chunksize=8;
							chunks=1;
							break;
						case 0x20: // 16 bit
							chunksize=16;
							if(len==4) chunks=2;
							else chunks = 1;
							break;
						case 0x40: // 32 bit
							chunksize=16;
							if(len==4) chunks=2;
							else chunks = 1;
							break;
						case 0x60: // undocumented guess (but works)
							chunksize=8;
							chunks=4;
							break;
						default:
							break;
					}

					const Bitu foremix = xga.foremix;
					const Bitu backmix = xga.backmix;
					const Bitu foresrcsel = (foremix >> 5) & 0x03;
					const Bitu backsrcsel = (backmix >> 5) & 0x03;
					const bool decoded_sources = (foresrcsel <= 1 && backsrcsel <= 1);
					const Bitu foresrc = foresrcsel ? xga.forecolor : xga.backcolor;
					const Bitu backsrc = backsrcsel ? xga.forecolor : xga.backcolor;
					
					for(Bitu k = 0; k < chunks; k++) { // chunks counter
						xga.waitcmd.newline = false;
						for(Bitu n = 0; n < chunksize; n++) { // pixels
							// This formula can rule the world ;)
							const Bitu mask = 1u << ((((n&0xF8)+(8-(n&0x7)))-1)+chunksize*k);
							const bool use_fore = (val & mask) != 0;
							const Bitu selected_mix = use_fore ? foremix : backmix;
							Bitu selected_src;

							if(GCC_LIKELY(decoded_sources)) {
								selected_src = use_fore ? foresrc : backsrc;
							} else {
								switch((selected_mix >> 5) & 0x03) {
									case 0x00:
										selected_src = xga.backcolor;
										break;
									case 0x01:
										selected_src = xga.forecolor;
										break;
									default:
										LOG_MSG("XGA: DrawBlitWait: Unsupported src %x",
											(selected_mix >> 5) & 0x03);
										selected_src = 0;
										break;
								}
							}
							XGA_DrawWaitSub(selected_mix, selected_src);

							if((xga.waitcmd.cury<2048) &&
							  (xga.waitcmd.cury >= xga.waitcmd.y2)) {
								xga.waitcmd.wait = false;
								k=1000; // no more chunks
								break;
							}
							// next chunk goes to next line
							if(xga.waitcmd.newline) break; 
						} // pixels loop
					} // chunks loop
					break;
				}

				default:
					LOG_MSG("XGA: DrawBlitWait: Unhandled mixmode: %d", mixmode);
					break;
			} // switch mixmode
			break;
		default:
			LOG_MSG("XGA: Unhandled draw command %x", xga.waitcmd.cmd);
			break;
	}
}

static void XGA_BlitRect(Bitu val) {
	Bit32u xat, yat;
	Bitu srcdata;
	Bitu colorcmpdata;

	Bitu srcval = 0;

	Bits srcx, srcy, tarx, tary, dx, dy;

	dx = -1;
	dy = -1;

	if(((val >> 5) & 0x01) != 0) dx = 1;
	if(((val >> 7) & 0x01) != 0) dy = 1;

	switch(XGA_COLOR_MODE) {
		case M_LIN8:
			colorcmpdata = xga.color_compare & 0xfful;
			break;
		case M_LIN15:
		case M_LIN16:
			colorcmpdata = xga.color_compare & 0xfffful;
			break;
		case M_LIN32:
			colorcmpdata = xga.color_compare;
			break;
		default:
			colorcmpdata = 0;
			break;
	}

	const bool color_cmp_enabled = (xga.control1 & 0x100) != 0;
	const bool color_cmp_src_ne = (xga.control1 & 0x080) != 0;

	srcx = xga.curx;
	srcy = xga.cury;
	tarx = xga.destx;
	tary = xga.desty;

	Bitu mixselect = (xga.pix_cntl >> 6) & 0x3;
	Bitu mixmode = 0x67; /* Source is bitmap data, mix mode is src */
	switch(mixselect) {
		case 0x00: /* Foreground mix is always used */
			mixmode = xga.foremix;
			break;
		case 0x02: /* CPU Data determines mix used */
			LOG_MSG("XGA: DrawPattern: Mixselect data from PIX_TRANS register");
			break;
		case 0x03: /* Video memory determines mix */
			//LOG_MSG("XGA: Srcdata: %x, Forecolor %x, Backcolor %x, Foremix: %x Backmix: %x", srcdata, xga.forecolor, xga.backcolor, xga.foremix, xga.backmix);
			break;
		default:
			LOG_MSG("XGA: BlitRect: Unknown mix select register");
			break;
	}


	/* With fixed FOREMIX the source selector is invariant. The solid forms do
	 * not depend on source VRAM at all; the bitmap form only needs one source
	 * read per pixel. All other modes retain the original general path. */
	if(mixselect == 0x00) {
		switch((mixmode >> 5) & 0x03) {
			case 0x00:
				XGA_BlitRectConstantSource(dx, dy, mixmode, xga.backcolor,
				                           colorcmpdata, color_cmp_enabled,
				                           color_cmp_src_ne);
				return;
			case 0x01:
				XGA_BlitRectConstantSource(dx, dy, mixmode, xga.forecolor,
				                           colorcmpdata, color_cmp_enabled,
				                           color_cmp_src_ne);
				return;
			case 0x03:
				XGA_BlitRectBitmapSource(dx, dy, mixmode, colorcmpdata,
				                         color_cmp_enabled, color_cmp_src_ne);
				return;
			default:
				break; /* PIX_TRANS source keeps the original fallback */
		}
	}

	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;

	/* Copy source to video ram */
	for(yat=0;yat<=height ;yat++) {
		srcx = xga.curx;
		tarx = xga.destx;

		for(xat=0;xat<=width;xat++) {
			srcdata = XGA_GetPoint(srcx, srcy);

			if(mixselect == 0x3) {
				if(srcdata == xga.forecolor) {
					mixmode = xga.foremix;
				} else {
					if(srcdata == xga.backcolor) {
						mixmode = xga.backmix;
					} else {
						/* Best guess otherwise */
						mixmode = 0x67; /* Source is bitmap data, mix mode is src */
					}
				}
			}

			switch((mixmode >> 5) & 0x03) {
				case 0x00: /* Src is background color */
					srcval = xga.backcolor;
					break;
				case 0x01: /* Src is foreground color */
					srcval = xga.forecolor;
					break;
				case 0x02: /* Src is pixel data from PIX_TRANS register */
					LOG_MSG("XGA: DrawPattern: Wants data from PIX_TRANS register");
					break;
				case 0x03: /* Src is bitmap data */
					srcval = srcdata;
					break;
				default:
					LOG_MSG("XGA: DrawPattern: Shouldn't be able to get here!");
					srcval = 0;
					break;
			}

			/* MULT_MISC bit 8 enables COLOR_CMP. Bit 7 (SRC_NE)
			 * selects whether equal or non-equal source pixels are skipped. */
			XGA_BlitWritePixel(tarx, tary, mixmode, srcval, colorcmpdata,
			                   color_cmp_enabled, color_cmp_src_ne);

			srcx += dx;
			tarx += dx;
		}
		srcy += dy;
		tary += dy;
	}
}

/* Fixed FOREMIX with a solid source does not use the 8x8 pattern bitmap.
 * This removes one source-VRAM read for every destination pixel. */
static void XGA_DrawSolidPattern(Bits dx, Bits dy, Bitu mixmode, Bitu srcval) {
	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;
	Bits tary = xga.desty;

	if((mixmode & 0x0f) == 0x03)
		return;

	if((mixmode & 0x0f) == 0x07) {
		if(XGA_TryFastSolidFill8(xga.destx, xga.desty, dx, dy,
		                            width + 1u, height + 1u, srcval))
			return;
		for(Bit32u yat = 0; yat <= height; ++yat) {
			Bits tarx = xga.destx;
			for(Bit32u xat = 0; xat <= width; ++xat) {
				XGA_DrawPoint(tarx, tary, srcval);
				tarx += dx;
			}
			tary += dy;
		}
		return;
	}

	for(Bit32u yat = 0; yat <= height; ++yat) {
		Bits tarx = xga.destx;
		for(Bit32u xat = 0; xat <= width; ++xat) {
			XGA_DrawMixedPoint(tarx, tary, mixmode, srcval);
			tarx += dx;
		}
		tary += dy;
	}
}

/* Fixed FOREMIX with a bitmap source keeps the same source selector and
 * ROP for the complete pattern command. */
static void XGA_DrawBitmapPattern(Bits dx, Bits dy, Bitu mixmode) {
	const Bit32u height = xga.MIPcount;
	const Bit32u width = xga.MAPcount;
	const Bits srcx = xga.curx;
	const Bits srcy = xga.cury;
	Bits tary = xga.desty;

	if((mixmode & 0x0f) == 0x03)
		return;

	if((mixmode & 0x0f) == 0x07) {
		for(Bit32u yat = 0; yat <= height; ++yat) {
			Bits tarx = xga.destx;
			for(Bit32u xat = 0; xat <= width; ++xat) {
				const Bitu srcval = XGA_GetPoint(srcx + (tarx & 0x7),
				                                  srcy + (tary & 0x7));
				XGA_DrawPoint(tarx, tary, srcval);
				tarx += dx;
			}
			tary += dy;
		}
		return;
	}

	for(Bit32u yat = 0; yat <= height; ++yat) {
		Bits tarx = xga.destx;
		for(Bit32u xat = 0; xat <= width; ++xat) {
			const Bitu srcval = XGA_GetPoint(srcx + (tarx & 0x7),
			                                  srcy + (tary & 0x7));
			XGA_DrawMixedPoint(tarx, tary, mixmode, srcval);
			tarx += dx;
		}
		tary += dy;
	}
}

static void XGA_DrawPattern(Bitu val) {
	Bitu srcdata;

	Bitu srcval = 0;
	Bitu destval;

	Bits xat, yat, srcx, srcy, tarx, tary, dx, dy;

	dx = -1;
	dy = -1;

	if(((val >> 5) & 0x01) != 0) dx = 1;
	if(((val >> 7) & 0x01) != 0) dy = 1;

	srcx = xga.curx;
	srcy = xga.cury;

	tary = xga.desty;

	Bitu mixselect = (xga.pix_cntl >> 6) & 0x3;
	Bitu mixmode = 0x67; /* Source is bitmap data, mix mode is src */
	switch(mixselect) {
		case 0x00: /* Foreground mix is always used */
			mixmode = xga.foremix;
			break;
		case 0x02: /* CPU Data determines mix used */
			LOG_MSG("XGA: DrawPattern: Mixselect data from PIX_TRANS register");
			break;
		case 0x03: /* Video memory determines mix */
			//LOG_MSG("XGA: Pixctl: %x, Srcdata: %x, Forecolor %x, Backcolor %x, Foremix: %x Backmix: %x",xga.pix_cntl, srcdata, xga.forecolor, xga.backcolor, xga.foremix, xga.backmix);
			break;
		default:
			LOG_MSG("XGA: DrawPattern: Unknown mix select register");
			break;
	}

	const Bits height = xga.MIPcount;
	const Bits width = xga.MAPcount;

	/* With fixed FOREMIX, BACKCOLOR and FORECOLOR are invariant and the
	 * pattern bitmap is not consulted at all. */
	if(mixselect == 0x00) {
		switch((mixmode >> 5) & 0x03) {
			case 0x00:
				XGA_DrawSolidPattern(dx, dy, mixmode, xga.backcolor);
				return;
			case 0x01:
				XGA_DrawSolidPattern(dx, dy, mixmode, xga.forecolor);
				return;
			case 0x03:
				XGA_DrawBitmapPattern(dx, dy, mixmode);
				return;
			default:
				break; /* preserve PIX_TRANS diagnostics in fallback */
		}
	}

	for(yat=0;yat<=height;yat++) {
		tarx = xga.destx;
		for(xat=0;xat<=width;xat++) {

			srcdata = XGA_GetPoint(srcx + (tarx & 0x7), srcy + (tary & 0x7));
			//LOG_MSG("patternpoint (%3d/%3d)v%x",srcx + (tarx & 0x7), srcy + (tary & 0x7),srcdata);
			if(mixselect == 0x3) {
				// TODO lots of guessing here but best results this way
				/*if(srcdata == xga.forecolor)*/ mixmode = xga.foremix;
				// else 
				if(srcdata == xga.backcolor || srcdata == 0) 
					mixmode = xga.backmix;
			}

			switch((mixmode >> 5) & 0x03) {
				case 0x00: /* Src is background color */
					srcval = xga.backcolor;
					break;
				case 0x01: /* Src is foreground color */
					srcval = xga.forecolor;
					break;
				case 0x02: /* Src is pixel data from PIX_TRANS register */
					LOG_MSG("XGA: DrawPattern: Wants data from PIX_TRANS register");
					break;
				case 0x03: /* Src is bitmap data */
					srcval = srcdata;
					break;
				default:
					LOG_MSG("XGA: DrawPattern: Shouldn't be able to get here!");
					srcval = 0;
					break;
			}

			destval = XGA_GetMixResult(mixmode, srcval, tarx, tary);

			XGA_DrawPoint(tarx, tary, destval);
			
			tarx += dx;
		}
		tary += dy;
	}
}

static void XGA_DrawCmd(Bitu val, Bitu len) {
	(void)len;
	Bit16u cmd;
	cmd = val >> 13;
#if XGA_SHOW_COMMAND_TRACE == 1
	//LOG_MSG("XGA: Draw command %x", cmd);
#endif
	xga.curcommand = val;
	switch(cmd) {
		case 1: /* Draw line */
			if((val & 0x100) == 0) {
				if((val & 0x8) == 0) {
#if XGA_SHOW_COMMAND_TRACE == 1
					LOG_MSG("XGA: Drawing Bresenham line");
#endif
                    XGA_DrawLineBresenham(val);
				} else {
#if XGA_SHOW_COMMAND_TRACE == 1
					LOG_MSG("XGA: Drawing vector line");
#endif
					XGA_DrawLineVector(val);
				}
			} else {
				LOG_MSG("XGA: Wants line drawn from PIX_TRANS register!");
			}
			break;
		case 2: /* Rectangle fill */
			if((val & 0x100) == 0) {
				xga.waitcmd.wait = false;
#if XGA_SHOW_COMMAND_TRACE == 1
				LOG_MSG("XGA: Draw immediate rect: xy(%3d/%3d), len(%3d/%3d)",
					xga.curx,xga.cury,xga.MAPcount,xga.MIPcount);
#endif
				XGA_DrawRectangle(val);

			} else {
				
				xga.waitcmd.newline = true;
				xga.waitcmd.wait = true;
				xga.waitcmd.curx = xga.curx;
				xga.waitcmd.cury = xga.cury;
				xga.waitcmd.x1 = xga.curx;
				xga.waitcmd.y1 = xga.cury;
				xga.waitcmd.x2 = (Bit16u)((xga.curx + xga.MAPcount)&0x0fff);
				xga.waitcmd.y2 = (Bit16u)((xga.cury + xga.MIPcount + 1)&0x0fff);
				xga.waitcmd.sizex = xga.MAPcount;
				xga.waitcmd.sizey = xga.MIPcount + 1;
				xga.waitcmd.cmd = 2;
				xga.waitcmd.buswidth = vga.mode | ((val&0x600) >> 4);
				xga.waitcmd.data = 0;
				xga.waitcmd.datasize = 0;

#if XGA_SHOW_COMMAND_TRACE == 1
				LOG_MSG("XGA: Draw wait rect, w/h(%3d/%3d), x/y1(%3d/%3d), x/y2(%3d/%3d), %4x",
					xga.MAPcount+1, xga.MIPcount+1,xga.curx,xga.cury,
					(xga.curx + xga.MAPcount)&0x0fff,
					(xga.cury + xga.MIPcount + 1)&0x0fff,val&0xffff);
#endif
			
			}
			break;
		case 6: /* BitBLT */
#if XGA_SHOW_COMMAND_TRACE == 1
			LOG_MSG("XGA: Blit Rect");
#endif
			XGA_BlitRect(val);
			break;
		case 7: /* Pattern fill */
#if XGA_SHOW_COMMAND_TRACE == 1
			LOG_MSG("XGA: Pattern fill: src(%3d/%3d), dest(%3d/%3d), fill(%3d/%3d)",
				xga.curx,xga.cury,xga.destx,xga.desty,xga.MAPcount,xga.MIPcount);
#endif
			XGA_DrawPattern(val);
			break;
		default:
			LOG_MSG("XGA: Unhandled draw command %x", cmd);
			break;
	}
}

static void XGA_SetDualReg(Bit32u& reg, Bitu val) {
	switch(XGA_COLOR_MODE) {
	case M_LIN8:
		reg = (Bit8u)(val&0xff); break;
	case M_LIN15:
	case M_LIN16:
		reg = (Bit16u)(val&0xffff); break;
	case M_LIN32:
		if (xga.control1 & 0x200)
			reg = val;
		else if (xga.control1 & 0x10)
			reg = (reg&0x0000ffff)|(val<<16);
		else
			reg = (reg&0xffff0000)|(val&0x0000ffff);
		xga.control1 ^= 0x10;
		break;
	}
}

static Bitu XGA_GetDualReg(Bit32u reg) {
	switch(XGA_COLOR_MODE) {
	case M_LIN8:
		return (Bit8u)(reg&0xff);
	case M_LIN15: case M_LIN16:
		return (Bit16u)(reg&0xffff);
	case M_LIN32:
		if (xga.control1 & 0x200) return reg;
		xga.control1 ^= 0x10;
		if (xga.control1 & 0x10) return reg&0x0000ffff;
		else return reg>>16;
	}
	return 0;
}

extern Bitu vga_read_p3da(Bitu port,Bitu iolen);

extern void vga_write_p3d4(Bitu port,Bitu val,Bitu iolen);
extern Bitu vga_read_p3d4(Bitu port,Bitu iolen);

extern void vga_write_p3d5(Bitu port,Bitu val,Bitu iolen);
extern Bitu vga_read_p3d5(Bitu port,Bitu iolen);

void XGA_Write(Bitu port, Bitu val, Bitu len) {
//	LOG_MSG("XGA: Write to port %x, val %8x, len %x", port,val, len);

	switch(port) {
		case 0x8100:// drawing control: row (low word), column (high word)
					// "CUR_X" and "CUR_Y" (see PORT 82E8h,PORT 86E8h)
			xga.cury = val & 0x0fff;
			if(len==4) xga.curx = (val>>16)&0x0fff;
			break;
		case 0x8102:
			xga.curx = val& 0x0fff;
			break;

		case 0x8108:// DWORD drawing control: destination Y and axial step
					// constant (low word), destination X and axial step
					// constant (high word) (see PORT 8AE8h,PORT 8EE8h)
			xga.desty = val&0x3FFF;
			if(len==4) xga.destx = (val>>16)&0x3fff;
			break;
		case 0x810a:
			xga.destx = val&0x3fff;
			break;
		case 0x8110: // WORD error term (see PORT 92E8h)
			xga.ErrTerm = val&0x3FFF;
			break;

		case 0x8120: // packed MMIO: DWORD background color (see PORT A2E8h)
			xga.backcolor = val;
			break;
		case 0x8124: // packed MMIO: DWORD foreground color (see PORT A6E8h)
			xga.forecolor = val;
			break;
		case 0x8128: // DWORD	write mask (see PORT AAE8h)
			xga.writemask = val;
			break;
		case 0x812C: // DWORD	read mask (see PORT AEE8h)
			xga.readmask = val;
			break;
		case 0x8134: // packed MMIO: DWORD	background mix (low word) and
					 // foreground mix (high word)	(see PORT B6E8h,PORT BAE8h)
			xga.backmix = val&0xFFFF;
			if(len==4) xga.foremix = (val>>16);
			break;
		case 0x8136:
			xga.foremix = val;
			break;
		case 0x8138:// DWORD top scissors (low word) and left scissors (high
					// word) (see PORT BEE8h,#P1047)
			xga.scissors.y1=val&0x0fff;
			if(len==4) xga.scissors.x1 = (val>>16)&0x0fff;
			break;
		case 0x813a:
			xga.scissors.x1 = val&0x0fff;
			break;
		case 0x813C:// DWORD bottom scissors (low word) and right scissors
					// (high word) (see PORT BEE8h,#P1047)
			xga.scissors.y2=val&0x0fff;
			if(len==4) xga.scissors.x2 = (val>>16)&0x0fff;
			break;
		case 0x813e:
			xga.scissors.x2 = val&0x0fff;
			break;

		case 0x8140:// DWORD data manipulation control (low word) and
					// miscellaneous 2 (high word) (see PORT BEE8h,#P1047)
			xga.pix_cntl=val&0xFFFF;
			if(len==4) xga.control2=(val>>16)&0x0fff;
			break;
		case 0x8144:// DWORD miscellaneous (low word) and read register select
					// (high word)(see PORT BEE8h,#P1047)
			xga.control1=val&0xffff;
			if(len==4)xga.read_sel=(val>>16)&0x7;
			break; 
		case 0x8148:// DWORD minor axis pixel count (low word) and major axis
					// pixel count (high word) (see PORT BEE8h,#P1047,PORT 96E8h)
			xga.MIPcount = val&0x0fff;
			if(len==4) xga.MAPcount = (val>>16)&0x0fff;
			break;
		case 0x814a:
			xga.MAPcount = val&0x0fff;
			break;
		case 0x92e8:
			xga.ErrTerm = val&0x3FFF;
			break;
		case 0x96e8:
			xga.MAPcount = val&0x0fff;
			break;
		case 0x9ae8:
		case 0x8118: // Trio64V+ packed MMIO
			XGA_DrawCmd(val, len);
			break;
		case 0xa2e8:
			XGA_SetDualReg(xga.backcolor, val);
			break;
		case 0xa6e8:
			XGA_SetDualReg(xga.forecolor, val);
			break;
		case 0xaae8:
			XGA_SetDualReg(xga.writemask, val);
			break;
		case 0xaee8:
			XGA_SetDualReg(xga.readmask, val);
			break;
		case 0x82e8:
			xga.cury = val&0x0fff;
			break;
		case 0x86e8:
			xga.curx = val&0x0fff;
			break;
		case 0x8ae8:
			xga.desty = val&0x3fff;
			break;
		case 0x8ee8:
			xga.destx = val&0x3fff;
			break;
		case 0xb2e8:
			XGA_SetDualReg(xga.color_compare, val);
			break;
		case 0xb6e8:
			xga.backmix = val;
			break;
		case 0xbae8:
			xga.foremix = val;
			break;
		case 0xbee8:
			XGA_Write_Multifunc(val, len);
			break;
		case 0xe2e8:
			xga.waitcmd.newline = false;
			XGA_DrawWait(val, len);
			break;
		case 0x83d4:
			if(len==1) vga_write_p3d4(0,val,1);
			else if(len==2) {
				vga_write_p3d4(0,val&0xff,1);
				vga_write_p3d5(0,val>>8,1);
			}
			else E_Exit("unimplemented XGA MMIO");
			break;
		case 0x83d5:
			if(len==1) vga_write_p3d5(0,val,1);
			else E_Exit("unimplemented XGA MMIO");
			break;
		default:
			if(port <= 0x4000) {
				//LOG_MSG("XGA: Wrote to port %4x with %08x, len %x", port, val, len);
				xga.waitcmd.newline = false;
				XGA_DrawWait(val, len);
				
			}
			else LOG_MSG("XGA: Wrote to port %x with %x, len %x", port, val, len);
			break;
	}
}

Bitu XGA_Read(Bitu port, Bitu len) {
	switch(port) {
		case 0x8118:
		case 0x9ae8:
			return 0x400; // nothing busy
			break;
		case 0x81ec: // S3 video data processor
			return 0x00007000; 
			break;
		case 0x83da:
			{
				Bits delaycyc = CPU_CycleMax/5000;
				if(GCC_UNLIKELY(CPU_Cycles < 3*delaycyc)) delaycyc = 0;
				CPU_Cycles -= delaycyc;
				CPU_IODelayRemoved += delaycyc;
				return vga_read_p3da(0,0);
				break;
			}
		case 0x83d4:
			if(len==1) return vga_read_p3d4(0,0);
			else E_Exit("unimplemented XGA MMIO");
			break;
		case 0x83d5:
			if(len==1) return vga_read_p3d5(0,0);
			else E_Exit("unimplemented XGA MMIO");
			break;
		case 0x9ae9:
			if(xga.waitcmd.wait) return 0x4;
			else return 0x0;
		case 0xbee8:
			return XGA_Read_Multifunc();
		case 0xb2e8:
			return XGA_GetDualReg(xga.color_compare);
		case 0xa2e8:
			return XGA_GetDualReg(xga.backcolor);
			break;
		case 0xa6e8:
			return XGA_GetDualReg(xga.forecolor);
			break;
		case 0xaae8:
			return XGA_GetDualReg(xga.writemask);
			break;
		case 0xaee8:
			return XGA_GetDualReg(xga.readmask);
			break;
		default:
			//LOG_MSG("XGA: Read from port %x, len %x", port, len);
			break;
	}
	return 0xffffffff; 
}

void VGA_SetupXGA(void) {
	if (!IS_VGA_ARCH) return;

	memset(&xga, 0, sizeof(XGAStatus));

	xga.scissors.y1 = 0;
	xga.scissors.x1 = 0;
	xga.scissors.y2 = 0xFFF;
	xga.scissors.x2 = 0xFFF;
	
	IO_RegisterWriteHandler(0x42e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x42e8,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x46e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x4ae8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	
	IO_RegisterWriteHandler(0x82e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x82e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x82e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x82e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x86e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x86e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x86e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x86e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x8ae8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x8ae8,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x8ee8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x8ee8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x8ee9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x8ee9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x92e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x92e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x92e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x92e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x96e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x96e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x96e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x96e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x9ae8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x9ae8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x9ae9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x9ae9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0x9ee8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x9ee8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0x9ee9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0x9ee9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xa2e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xa2e8,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xa6e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xa6e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xa6e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xa6e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xaae8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xaae8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xaae9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xaae9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xaee8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xaee8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xaee9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xaee9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xb2e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xb2e8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xb2e9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xb2e9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xb6e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xb6e8,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xbee8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xbee8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xbee9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xbee9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xbae8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xbae8,&XGA_Read,IO_MB | IO_MW | IO_MD);
	IO_RegisterWriteHandler(0xbae9,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xbae9,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xe2e8,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xe2e8,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xe2e0,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xe2e0,&XGA_Read,IO_MB | IO_MW | IO_MD);

	IO_RegisterWriteHandler(0xe2ea,&XGA_Write,IO_MB | IO_MW | IO_MD);
	IO_RegisterReadHandler(0xe2ea,&XGA_Read,IO_MB | IO_MW | IO_MD);
}

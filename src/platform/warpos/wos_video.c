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

/* Native WarpOS screen and input, see wos_video.h. Same rules as OpenSWOS's
 * platform_wos.c: no SA_BitMap (the card owns its memory), nothing but the
 * copy between LockBitMapTagList and UnLockBitMap, one SetSignal per poll. */

#include <string.h>

#pragma pack(push,2)
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <devices/inputevent.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#pragma pack(pop)

#include "wos_video.h"

/* Defined by the SDK's SDL; opened here when SDL's video is not used. */
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
extern struct Library *CyberGfxBase;

static int opened_libs;
static struct Screen *scr;
static struct Window *win;
static UWORD *blank_pointer;
static int scr_w, scr_h;
static ULONG scr_id = INVALID_ID;
static struct WOS_VideoFormat scr_fmt;
static const char *last_error = "";

#define WOS_EVQ_SIZE 256
static struct WOS_VideoEvent evq[WOS_EVQ_SIZE];
static int evq_head, evq_tail;

const char *WOS_Video_Error(void) { return last_error; }
int WOS_Video_Width(void) { return scr_w; }
int WOS_Video_Height(void) { return scr_h; }

static int open_libs(void) {
	if (!IntuitionBase) {
		IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
		opened_libs |= 1;
	}
	if (!GfxBase) {
		GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
		opened_libs |= 2;
	}
	if (!CyberGfxBase) {
		/* v41: LockBitMapTagList; Picasso96's CyberGraphX layer reports it. */
		CyberGfxBase = OpenLibrary(CYBERGFXNAME, 41);
		opened_libs |= 4;
	}
	if (!IntuitionBase || !GfxBase) { last_error = "intuition/graphics.library v39 missing"; return -1; }
	if (!CyberGfxBase) { last_error = "no cybergraphics.library v41 (an RTG card is needed)"; return -1; }
	return 0;
}

static void close_libs(void) {
	if ((opened_libs & 4) && CyberGfxBase) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
	if ((opened_libs & 2) && GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
	if ((opened_libs & 1) && IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
	opened_libs = 0;
}

static int format_of(ULONG pixfmt, struct WOS_VideoFormat *f) {
	switch (pixfmt) {
	case PIXFMT_RGB15:   f->bits = 15; f->bgr = 0; f->swap = 0; return 1;
	case PIXFMT_BGR15:   f->bits = 15; f->bgr = 1; f->swap = 0; return 1;
	case PIXFMT_RGB15PC: f->bits = 15; f->bgr = 0; f->swap = 1; return 1;
	case PIXFMT_BGR15PC: f->bits = 15; f->bgr = 1; f->swap = 1; return 1;
	case PIXFMT_RGB16:   f->bits = 16; f->bgr = 0; f->swap = 0; return 1;
	case PIXFMT_BGR16:   f->bits = 16; f->bgr = 1; f->swap = 0; return 1;
	case PIXFMT_RGB16PC: f->bits = 16; f->bgr = 0; f->swap = 1; return 1;
	case PIXFMT_BGR16PC: f->bits = 16; f->bgr = 1; f->swap = 1; return 1;
	default: return 0;
	}
}

/* A 15/16-bit RTG mode of at least w x h. BestCModeIDTagList never fails: it
 * answers a native chipset mode when nothing fits, so every answer is checked. */
static ULONG find_mode(int w, int h) {
	static const int ladder[][2] = { {0, 0}, {640, 480}, {800, 600}, {1024, 768} };
	unsigned i;
	for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		struct TagItem tags[4];
		struct WOS_VideoFormat f;
		ULONG id;
		const int tw = i ? ladder[i][0] : w, th = i ? ladder[i][1] : h;
		if (tw < w || th < h) continue;
		tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)tw;
		tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)th;
		tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = 16UL;
		tags[3].ti_Tag = TAG_END;                 tags[3].ti_Data = 0UL;
		id = BestCModeIDTagList(tags);
		if (id == (ULONG)INVALID_ID || !IsCyberModeID(id)) continue;
		if (!format_of(GetCyberIDAttr(CYBRIDATTR_PIXFMT, id), &f)) continue;
		if ((int)GetCyberIDAttr(CYBRIDATTR_WIDTH, id) < w ||
		    (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) < h) continue;
		return id;
	}
	return (ULONG)INVALID_ID;
}

static void close_screen(void) {
	if (win) {
		struct IntuiMessage *m;
		while ((m = (struct IntuiMessage *)GetMsg(win->UserPort)))
			ReplyMsg((struct Message *)m);
		if (blank_pointer) ClearPointer(win);
		CloseWindow(win);
		win = NULL;
	}
	if (scr) {
		CloseScreen(scr);
		scr = NULL;
	}
	scr_w = scr_h = 0;
	scr_id = INVALID_ID;
}

int WOS_Video_Open(int w, int h, struct WOS_VideoFormat *fmt) {
	ULONG id;
	if (!opened_libs && open_libs() != 0) return -1;
	id = find_mode(w, h);
	if (id == (ULONG)INVALID_ID) {
		last_error = "no 15/16-bit RTG screen mode large enough";
		return -1;
	}
	if (scr && id == scr_id) {	/* the open screen already fits */
		*fmt = scr_fmt;
		return 0;
	}
	close_screen();

	scr_w = (int)GetCyberIDAttr(CYBRIDATTR_WIDTH, id);
	scr_h = (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, id);
	scr = OpenScreenTags(NULL,
	                     SA_Width, (ULONG)scr_w,
	                     SA_Height, (ULONG)scr_h,
	                     SA_Depth, 16UL,
	                     SA_Type, (ULONG)CUSTOMSCREEN,
	                     SA_Quiet, TRUE,
	                     SA_ShowTitle, FALSE,
	                     SA_Title, (ULONG)"DOSBox",
	                     SA_DisplayID, id,
	                     TAG_END);
	if (!scr) {
		last_error = "OpenScreen failed";
		scr_w = scr_h = 0;
		return -1;
	}
	/* The screen's real format, which a 16-bit request may not have got. */
	if (!format_of(GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_PIXFMT), &scr_fmt)) {
		last_error = "the RTG screen is not 15/16-bit";
		close_screen();
		return -1;
	}
	win = OpenWindowTags(NULL,
	                     WA_CustomScreen, (ULONG)scr,
	                     WA_Left, 0UL, WA_Top, 0UL,
	                     WA_Width, (ULONG)scr_w, WA_Height, (ULONG)scr_h,
	                     WA_Flags, (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_ACTIVATE |
	                                       WFLG_RMBTRAP | WFLG_REPORTMOUSE),
	                     WA_IDCMP, (ULONG)(IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS |
	                                       IDCMP_MOUSEMOVE | IDCMP_DELTAMOVE),
	                     TAG_END);
	if (!win) {
		last_error = "OpenWindow failed";
		close_screen();
		return -1;
	}
	ActivateWindow(win);	/* WFLG_ACTIVATE alone is not reliable */
	ScreenToFront(scr);
	if (!blank_pointer)
		blank_pointer = (UWORD *)AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
	if (blank_pointer)
		SetPointer(win, blank_pointer, 1, 16, 0, 0);
	SetRast(&scr->RastPort, 0);
	scr_id = id;
	evq_head = evq_tail = 0;
	*fmt = scr_fmt;
	return 0;
}

void WOS_Video_Close(void) {
	close_screen();
	if (blank_pointer) {
		FreeVec(blank_pointer);
		blank_pointer = NULL;
	}
	close_libs();
}

void WOS_Video_ToFront(void) {
	if (scr) {
		ScreenToFront(scr);
		if (win) ActivateWindow(win);
	}
}

void WOS_Video_Clear(void) {
	if (scr) SetRast(&scr->RastPort, 0);
}

/* Plain word stores only: newlib's memcpy may use dcbz/dcba, which the card's
 * uncached memory does not take. */
static void copy_row(ULONG *d, const ULONG *s, int words, int swap) {
	int i;
	if (swap) {
		for (i = 0; i < words; i++) {
			const ULONG v = s[i];
			d[i] = ((v & 0x00ff00ffUL) << 8) | ((v >> 8) & 0x00ff00ffUL);
		}
	} else {
		for (i = 0; i < words; i++) d[i] = s[i];
	}
}

void WOS_Video_Present(const void *pic, int pitch, int w, int x, int y,
                       const int *ranges, int nranges) {
	struct TagItem t[3];
	APTR handle;
	UBYTE *base = NULL;
	ULONG bpr = 0;
	int r;
	if (!scr || nranges <= 0) return;
	t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
	t[1].ti_Tag = LBMI_BYTESPERROW; t[1].ti_Data = (ULONG)&bpr;
	t[2].ti_Tag = TAG_END;          t[2].ti_Data = 0;
	handle = LockBitMapTagList((APTR)scr->RastPort.BitMap, t);
	if (!handle) return;
	if (base) {
		/* 2-byte pixels, x even for 4-byte alignment (the caller centres on
		 * even columns); an odd last pixel is copied as a halfword. */
		const int words = w >> 1;
		for (r = 0; r < nranges; r++) {
			int row = ranges[r * 2], n = ranges[r * 2 + 1];
			for (; n > 0; n--, row++) {
				const UBYTE *s = (const UBYTE *)pic + row * pitch;
				UBYTE *d = base + (y + row) * bpr + x * 2;
				copy_row((ULONG *)d, (const ULONG *)s, words, scr_fmt.swap);
				if (w & 1) {
					UWORD v = ((const UWORD *)s)[w - 1];
					if (scr_fmt.swap) v = (UWORD)((v << 8) | (v >> 8));
					((UWORD *)d)[w - 1] = v;
				}
			}
		}
	}
	UnLockBitMap(handle);
}

static void push_event(int type, int code, int down, int dx, int dy, unsigned qual) {
	const int next = (evq_tail + 1) % WOS_EVQ_SIZE;
	if (next == evq_head) return;	/* full: drop */
	evq[evq_tail].type = type;
	evq[evq_tail].code = code;
	evq[evq_tail].down = down;
	evq[evq_tail].dx = dx;
	evq[evq_tail].dy = dy;
	evq[evq_tail].qual = qual;
	evq_tail = next;
}

int WOS_Video_Poll(void) {
	struct IntuiMessage *m;
	ULONG port_sig, sigs;
	int n = 0;
	if (!win) return 0;
	port_sig = 1UL << win->UserPort->mp_SigBit;
	sigs = SetSignal(0L, port_sig | SIGBREAKF_CTRL_C);	/* read and clear both */
	if (sigs & port_sig) {
		while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
			const ULONG cls = m->Class;
			const UWORD code = m->Code, qual = m->Qualifier;
			const WORD mx = m->MouseX, my = m->MouseY;
			ReplyMsg((struct Message *)m);
			switch (cls) {
			case IDCMP_RAWKEY:
				if ((code & 0x7f) >= 0x68 && (code & 0x7f) <= 0x6a) break;	/* mouse button codes */
				if (qual & IEQUALIFIER_REPEAT) break;	/* DOSBox repeats keys itself */
				push_event(WOS_EV_KEY, code & 0x7f, !(code & IECODE_UP_PREFIX), 0, 0, qual);
				n++;
				break;
			case IDCMP_MOUSEMOVE:
				if (!mx && !my) break;
				push_event(WOS_EV_MOTION, 0, 0, mx, my, qual);
				n++;
				break;
			case IDCMP_MOUSEBUTTONS: {
				int b;
				switch (code & ~IECODE_UP_PREFIX) {
				case IECODE_LBUTTON: b = 1; break;
				case IECODE_MBUTTON: b = 2; break;
				case IECODE_RBUTTON: b = 3; break;
				default: b = 0; break;
				}
				if (!b) break;
				push_event(WOS_EV_BUTTON, b, !(code & IECODE_UP_PREFIX), 0, 0, qual);
				n++;
				break;
			}
			default:
				break;
			}
		}
	}
	if (sigs & SIGBREAKF_CTRL_C) {
		push_event(WOS_EV_QUIT, 0, 0, 0, 0, 0);
		n++;
	}
	return n;
}

int WOS_Video_NextEvent(struct WOS_VideoEvent *ev) {
	if (evq_head == evq_tail) return 0;
	*ev = evq[evq_head];
	evq_head = (evq_head + 1) % WOS_EVQ_SIZE;
	return 1;
}

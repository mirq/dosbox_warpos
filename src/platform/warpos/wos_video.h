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

/* Native WarpOS screen and input for the SDL frontend (src/gui/sdlmain.cpp).
 * WarpOS SDL turns every frame and every event poll into several PPC->68k
 * calls (a lock, a blit, an update, a GetMsg plus a lowlevel joystick read).
 * This layer keeps it to what the OS really needs: one LockBitMapTagList /
 * UnLockBitMap per presented frame with only the changed rows copied, and
 * one SetSignal per event poll, with GetMsg only when the window's port has
 * something. The AmigaOS headers need 2-byte packing, so this is C. */

#ifndef DOSBOX_WOS_VIDEO_H
#define DOSBOX_WOS_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/* The opened screen's pixel layout, for building DOSBox's 15/16-bit colour
 * values. The renderer writes native big-endian words; for the little-endian
 * ("PC") card formats WOS_Video_Present swaps the bytes while copying. */
struct WOS_VideoFormat {
	int bits;	/* 15 or 16 */
	int bgr;	/* blue in the high bits */
	int swap;	/* little-endian on the card */
};

/* Opens (or keeps, when it already fits) a 15/16-bit RTG screen of at least
 * w x h with a backdrop window for input; the picture is centred on it.
 * Returns 0 and fills *fmt, or -1 with a reason in WOS_Video_Error(). */
int WOS_Video_Open(int w, int h, struct WOS_VideoFormat *fmt);
void WOS_Video_Close(void);
const char *WOS_Video_Error(void);
int WOS_Video_Width(void);
int WOS_Video_Height(void);
void WOS_Video_ToFront(void);

/* Copies rows of a picture in main memory (w pixels wide, 2 bytes each) to
 * the screen at (x, y) under a single bitmap lock. ranges holds pairs of
 * (first row, row count) in picture coordinates. */
void WOS_Video_Present(const void *pic, int pitch, int w, int x, int y,
                       const int *ranges, int nranges);
void WOS_Video_Clear(void);

enum {
	WOS_EV_KEY = 1,		/* code: Amiga raw key, down, qual */
	WOS_EV_MOTION,		/* dx, dy: relative motion */
	WOS_EV_BUTTON,		/* code: 1 left, 2 middle, 3 right (SDL numbering), down */
	WOS_EV_QUIT		/* CTRL-C sent to the program */
};

struct WOS_VideoEvent {
	int type;
	int code;
	int down;
	int dx, dy;
	unsigned int qual;
};

/* Reads the window's signal (and CTRL-C) with one SetSignal and, when set,
 * drains the port into an internal queue. Returns the number of events. */
int WOS_Video_Poll(void);
/* Pops the next queued event; returns 0 when the queue is empty. */
int WOS_Video_NextEvent(struct WOS_VideoEvent *ev);

#ifdef __cplusplus
}
#endif

#endif

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

/* Minimal time-base profiler for the WarpOS build. Enabled with
 * WOS_PROF=1 in the environment (ENV:WOS_PROF); every 5 seconds a line with
 * the share of wall time per bucket is appended to T:dbprof.txt. */

#ifndef DOSBOX_WOS_PROF_H
#define DOSBOX_WOS_PROF_H

#if defined(__WARPOS__)

enum {
	WOS_PROF_CPU,		/* cpudecoder */
	WOS_PROF_PIC,		/* PIC_RunQueue events (VGA draw, render, sound) */
	WOS_PROF_EVENTS,	/* GFX_Events */
	WOS_PROF_TICKS,		/* increaseticks, incl. SDL_Delay idle */
	WOS_PROF_STARTUPD,	/* GFX_StartUpdate */
	WOS_PROF_ENDUPD,	/* GFX_EndUpdate */
	WOS_PROF_PALETTE,	/* GFX_SetPalette */
	WOS_PROF_COUNT
};

extern unsigned long long wos_prof_acc[WOS_PROF_COUNT];
extern unsigned long wos_prof_frames;
extern int wos_prof_on;

static inline unsigned long long WOS_TimeBase(void) {
	unsigned long hi, lo, hi2;
	do {
		__asm__ __volatile__("mftbu %0" : "=r"(hi));
		__asm__ __volatile__("mftb %0" : "=r"(lo));
		__asm__ __volatile__("mftbu %0" : "=r"(hi2));
	} while (hi != hi2);
	return ((unsigned long long)hi << 32) | lo;
}

/* Called from the main loop; writes a report every 5 s when enabled. */
void WOS_Prof_Poll(void);

#define WOS_PROF_BEGIN(id) unsigned long long wos_prof_t0_##id = wos_prof_on ? WOS_TimeBase() : 0
#define WOS_PROF_END(id) do { if (wos_prof_on) wos_prof_acc[id] += WOS_TimeBase() - wos_prof_t0_##id; } while (0)

#else

#define WOS_PROF_BEGIN(id) do {} while (0)
#define WOS_PROF_END(id) do {} while (0)

#endif

#endif

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

/* WarpOS platform helpers shared by the DOSBox core (C and C++). */

#ifndef DOSBOX_WOS_PLATFORM_H
#define DOSBOX_WOS_PLATFORM_H

#include <stddef.h>

/* Every PPC that runs WarpOS (603e, 604e, G3, G4) has 32-byte cache lines. */
#define WOS_CACHE_LINE 32

/* Make freshly written code visible to instruction fetch: write the data
 * cache back to memory, then invalidate the matching instruction cache
 * lines. WarpOS has no CacheFlushDataInstArea(), and these instructions are
 * legal in user mode. */
static inline void WOS_FlushCodeCache(void *start, size_t len) {
	unsigned long p = (unsigned long)start & ~(unsigned long)(WOS_CACHE_LINE - 1);
	const unsigned long end = (unsigned long)start + len;
	unsigned long q;
	if (!len) return;
	for (q = p; q < end; q += WOS_CACHE_LINE)
		__asm__ __volatile__("dcbst 0,%0" : : "r"(q) : "memory");
	__asm__ __volatile__("sync" : : : "memory");
	for (q = p; q < end; q += WOS_CACHE_LINE)
		__asm__ __volatile__("icbi 0,%0" : : "r"(q) : "memory");
	__asm__ __volatile__("sync\n\tisync" : : : "memory");
}

#endif

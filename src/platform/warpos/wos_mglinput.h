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

/* Input from the MiniGL window. While the Voodoo display is up, MiniGL owns
 * the screen and SDL's window gets no events, so the Voodoo backend reads
 * the MiniGL window's IDCMP port through this small C layer (the AmigaOS
 * headers need 2-byte packing and stay out of the C++ code). */

#ifndef DOSBOX_WOS_MGLINPUT_H
#define DOSBOX_WOS_MGLINPUT_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
	WOS_MGL_EVENT_KEY = 1,		/* code: Amiga raw key (0x00-0x7f), down */
	WOS_MGL_EVENT_MOUSEMOVE,	/* dx, dy: relative motion */
	WOS_MGL_EVENT_BUTTON		/* code: 0 left, 1 right, 2 middle, down */
};

struct WOS_MGLEvent {
	int type;
	int code;
	int down;
	int dx, dy;
};

/* window: the struct Window * from mglGetWindowHandle(). Returns 0 on failure. */
int WOS_MGLInput_Attach(void *window);
void WOS_MGLInput_Detach(void);
/* Returns 1 and fills ev while messages are pending, 0 when drained. */
int WOS_MGLInput_Next(struct WOS_MGLEvent *ev);

#ifdef __cplusplus
}
#endif

#endif

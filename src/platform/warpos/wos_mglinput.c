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

/* MiniGL window input for the WarpOS Voodoo backend, see wos_mglinput.h.
 * Same approach as quake3amiga's amiga_glimp.c/amiga_input.c: widen the
 * window's IDCMP and drain its UserPort with plain exec GetMsg/ReplyMsg
 * (the Run68K gateway variant is not usable from a MOS2WOS PPC task). */

#pragma pack(push,2)
#include <exec/exec.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#pragma pack(pop)

#include "wos_mglinput.h"

/* Defined and opened by the SDK's SDL; opened here only if SDL has not. */
extern struct IntuitionBase *IntuitionBase;

static struct Window *mgl_window = NULL;
static int opened_intuition = 0;

int WOS_MGLInput_Attach(void *window) {
	if (!window) return 0;
	if (!IntuitionBase) {
		IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
		if (!IntuitionBase) return 0;
		opened_intuition = 1;
	}
	mgl_window = (struct Window *)window;
	ModifyIDCMP(mgl_window, IDCMP_RAWKEY|IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_DELTAMOVE);
	mgl_window->Flags |= WFLG_REPORTMOUSE;
	return 1;
}

void WOS_MGLInput_Detach(void) {
	struct IntuiMessage *imsg;
	if (mgl_window && mgl_window->UserPort) {
		while ((imsg = (struct IntuiMessage *)GetMsg(mgl_window->UserPort)))
			ReplyMsg((struct Message *)imsg);
	}
	mgl_window = NULL;
	if (opened_intuition) {
		CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		opened_intuition = 0;
	}
}

int WOS_MGLInput_Next(struct WOS_MGLEvent *ev) {
	struct IntuiMessage *imsg;
	if (!mgl_window || !mgl_window->UserPort) return 0;

	while ((imsg = (struct IntuiMessage *)GetMsg(mgl_window->UserPort))) {
		const ULONG cls = imsg->Class;
		const UWORD code = imsg->Code;
		const WORD dx = imsg->MouseX, dy = imsg->MouseY;
		ReplyMsg((struct Message *)imsg);

		switch (cls) {
		case IDCMP_RAWKEY:
			if ((code & 0x7f) >= 0x68 && (code & 0x7f) <= 0x6a) break;	/* mouse button keycodes */
			ev->type = WOS_MGL_EVENT_KEY;
			ev->code = code & 0x7f;
			ev->down = !(code & IECODE_UP_PREFIX);
			return 1;
		case IDCMP_MOUSEMOVE:
			if (!dx && !dy) break;
			ev->type = WOS_MGL_EVENT_MOUSEMOVE;
			ev->dx = dx;
			ev->dy = dy;
			return 1;
		case IDCMP_MOUSEBUTTONS:
			switch (code & ~IECODE_UP_PREFIX) {
			case IECODE_LBUTTON: ev->code = 0; break;
			case IECODE_RBUTTON: ev->code = 1; break;
			case IECODE_MBUTTON: ev->code = 2; break;
			default: continue;
			}
			ev->type = WOS_MGL_EVENT_BUTTON;
			ev->down = !(code & IECODE_UP_PREFIX);
			return 1;
		default:
			break;
		}
	}
	return 0;
}

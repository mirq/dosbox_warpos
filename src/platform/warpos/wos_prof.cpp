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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dosbox.h"
#include "cpu.h"
#include "timer.h"
#include "wos_prof.h"

unsigned long long wos_prof_acc[WOS_PROF_COUNT];
unsigned long wos_prof_frames;
int wos_prof_on = -1;

static const char *const wos_prof_names[WOS_PROF_COUNT] = {
	"cpu", "pic", "events", "ticks", "startupd", "endupd", "palette"
};

void WOS_Prof_Poll(void) {
	static Bit32u last_ms;
	static unsigned long long last_tb;
	if (wos_prof_on < 0) {
		const char *e = getenv("WOS_PROF");
		wos_prof_on = (e && e[0] == '1') ? 1 : 0;
		last_ms = GetTicks();
		last_tb = WOS_TimeBase();
		return;
	}
	if (!wos_prof_on) return;
	const Bit32u now_ms = GetTicks();
	if (now_ms - last_ms < 5000) return;

	const unsigned long long now_tb = WOS_TimeBase();
	const double total = (double)(now_tb - last_tb);
	FILE *f = fopen("T:dbprof.txt", "a");
	if (f) {
		fprintf(f, "%lums tb=%.0f/s frames=%lu cycles=%ld |",
		        (unsigned long)(now_ms - last_ms),
		        total * 1000.0 / (double)(now_ms - last_ms),
		        wos_prof_frames, (long)CPU_CycleMax);
		for (int i = 0; i < WOS_PROF_COUNT; i++)
			fprintf(f, " %s=%.1f%%", wos_prof_names[i], total > 0 ? 100.0 * (double)wos_prof_acc[i] / total : 0.0);
		fprintf(f, "\n");
		fclose(f);
	}
	memset(wos_prof_acc, 0, sizeof(wos_prof_acc));
	wos_prof_frames = 0;
	last_ms = now_ms;
	last_tb = WOS_TimeBase();
}

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

/* Runtime symbols the WarpOS C++ archives leave undefined.
 *
 * libstdc++.a(cow-stdexcept.o) carries the transactional-memory clones of
 * std::logic_error & co. They weakly reference the libitm entry points and
 * the TM clones of operator new[]/delete. Only code compiled with -fgnu-tm
 * can reach them and DOSBox has none, but Elf2Exe2 refuses any undefined
 * symbol, weak or not. Satisfy them with stubs that must never run. */

#include <stdlib.h>
#include <stddef.h>

static void wos_no_libitm(void) {
	abort();
}

void _ITM_addUserCommitAction(void *fn, unsigned long long tid, void *arg) { wos_no_libitm(); }
void _ITM_memcpyRnWt(void *dst, const void *src, size_t n) { wos_no_libitm(); }
void _ITM_memcpyRtWn(void *dst, const void *src, size_t n) { wos_no_libitm(); }
unsigned char _ITM_RU1(const unsigned char *p) { wos_no_libitm(); return 0; }
unsigned int _ITM_RU4(const unsigned int *p) { wos_no_libitm(); return 0; }
/* transaction clones of operator delete(void*) and operator new[](unsigned int) */
void _ZGTtdlPv(void *p) { wos_no_libitm(); }
void *_ZGTtnaj(size_t n) { wos_no_libitm(); return NULL; }

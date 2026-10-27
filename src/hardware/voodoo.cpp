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

/* DOSBox 0.74 glue for the 3dfx Voodoo emulation: [voodoo] config section,
 * the SST-1 PCI device and the 16MB LFB/register/texture memory window.
 * The emulation core (voodoo_emu.cpp, voodoo_interface.cpp, voodoo_*.h) is
 * imported from DOSBox-X commit 5a0a023a0d9f741a48faac897b8633b3c1c8e749. */

#include <string>

#include "dosbox.h"
#include "setup.h"
#include "mem.h"
#include "paging.h"
#include "pci_bus.h"
#include "voodoo.h"
#include "voodoo_interface.h"
#include "voodoo_emu.h"
#include "voodoo_opengl.h"

#if defined(PCI_FUNCTIONALITY_ENABLED)

static const Bit16u SST_VENDOR = 0x121a;	// 3dfx
static const Bit16u SST_DEVICE = 0x0001;	// Voodoo Graphics (SST-1)

class PCI_SSTDevice:public PCI_Device {
public:
	PCI_SSTDevice():PCI_Device(SST_VENDOR,SST_DEVICE) {
	}

	Bits ParseReadRegister(Bit8u regnum) {
		return regnum;
	}

	bool OverrideReadRegister(Bit8u regnum, Bit8u* rval, Bit8u* rval_mask) {
		return false;
	}

	Bits ParseWriteRegister(Bit8u regnum,Bit8u value) {
		if ((regnum>=0x14) && (regnum<0x28)) return -1;	// base addresses are read-only
		if ((regnum>=0x30) && (regnum<0x34)) return -1;	// expansion rom addresses are read-only
		switch (regnum) {
			/* BAR0: 16MB prefetchable memory, only the top byte is decoded */
			case 0x10:
				return 0x08;
			case 0x11:
			case 0x12:
				return 0x00;
			case 0x13:
				VOODOO_PCI_SetLFB((Bit32u)value<<24);
				return value;
			case 0x40:
				VOODOO_PCI_InitEnable(value&7);
				break;
			case 0x41:
			case 0x42:
			case 0x43:
				return -1;
			case 0xc0:
				VOODOO_PCI_Enable(true);
				return -1;
			case 0xe0:
				VOODOO_PCI_Enable(false);
				return -1;
			default:
				break;
		}
		return value;
	}

	bool InitializeRegisters(Bit8u registers[256]) {
		registers[0x08] = 0x02;	// revision
		registers[0x09] = 0x00;	// interface
		registers[0x0a] = 0x00;	// subclass code (video/graphics controller)
		registers[0x0b] = 0x04;	// class code (multimedia device)
		registers[0x0e] = 0x00;	// header type (other)

		// reset
		registers[0x04] = 0x02;	// command register (memory space enabled)
		registers[0x05] = 0x00;
		registers[0x06] = 0x80;	// status register (fast back-to-back)
		registers[0x07] = 0x00;

		registers[0x3c] = 0xff;	// no irq

		// BAR0: memory space, prefetchable
		Bit32u address_space = (((Bit32u)VOODOO_INITIAL_LFB)&0xfffffff0) | 0x08;
		registers[0x10] = (Bit8u)(address_space&0xff);
		registers[0x11] = (Bit8u)((address_space>>8)&0xff);
		registers[0x12] = (Bit8u)((address_space>>16)&0xff);
		registers[0x13] = (Bit8u)((address_space>>24)&0xff);
		return true;
	}
};


static Bits voodoo_emulation_type = VOODOO_EMU_TYPE_OFF;
static Bit32u voodoo_current_lfb = 0;

void VOODOO_PCI_InitEnable(Bitu val) {
	if (voodoo_emulation_type != VOODOO_EMU_TYPE_OFF) Voodoo_PCI_InitEnable(val);
}

void VOODOO_PCI_Enable(bool enable) {
	if (voodoo_emulation_type != VOODOO_EMU_TYPE_OFF) Voodoo_PCI_Enable(enable);
}

void VOODOO_PCI_SetLFB(Bit32u lfbaddr) {
	lfbaddr &= ~(Bit32u)(VOODOO_LFB_SIZE-1);
	if (voodoo_emulation_type == VOODOO_EMU_TYPE_OFF || lfbaddr == voodoo_current_lfb) return;
	voodoo_current_lfb = lfbaddr;
	if (lfbaddr) MEM_SetVoodooLFB(lfbaddr>>12, VOODOO_PAGES, Voodoo_GetPageHandler());
	else MEM_ClearVoodooLFB();
	LOG(LOG_PCI,LOG_NORMAL)("Voodoo LFB now at %x",lfbaddr);
}

static void VOODOO_ShutDown(Section* /*sec*/) {
	if (voodoo_emulation_type == VOODOO_EMU_TYPE_OFF) return;
	PCI_RemoveDevice(SST_VENDOR,SST_DEVICE);
	MEM_ClearVoodooLFB();
	Voodoo_Shut_Down();
	voodoo_emulation_type = VOODOO_EMU_TYPE_OFF;
	voodoo_current_lfb = 0;
}

void VOODOO_Init(Section* sec) {
	Section_prop * section=static_cast<Section_prop *>(sec);
	std::string card(section->Get_string("voodoo_card"));

	voodoo_emulation_type = VOODOO_EMU_TYPE_OFF;
	if (card == "software") {
		voodoo_emulation_type = VOODOO_EMU_TYPE_SOFTWARE;
	} else if (card == "minigl" || card == "opengl" || card == "auto") {
		voodoo_emulation_type = VOODOO_HAVE_OGL ? VOODOO_EMU_TYPE_ACCELERATED : VOODOO_EMU_TYPE_SOFTWARE;
	}
	if (voodoo_emulation_type == VOODOO_EMU_TYPE_OFF) return;

	/* 4MB: 2MB frame buffer + one 2MB TMU, 12MB: 4MB frame buffer + two 4MB TMUs */
	bool max_voodoomem = (section->Get_int("voodoo_memsize") == 12);
	Voodoo_Initialize(voodoo_emulation_type, 1, max_voodoomem);

	if (PCI_RegisterDevice(new PCI_SSTDevice()) == -1) {
		LOG_MSG("VOODOO: no free PCI slot, Voodoo disabled");
		Voodoo_Shut_Down();
		voodoo_emulation_type = VOODOO_EMU_TYPE_OFF;
		return;
	}
	VOODOO_PCI_SetLFB(VOODOO_INITIAL_LFB);
	sec->AddDestroyFunction(&VOODOO_ShutDown,false);
	LOG_MSG("VOODOO: %s emulation, %dMB, LFB at %x",
		voodoo_emulation_type == VOODOO_EMU_TYPE_ACCELERATED ? "accelerated" : "software",
		max_voodoomem ? 12 : 4, voodoo_current_lfb);
}

#else

void VOODOO_Init(Section* /*sec*/) {
}

#endif

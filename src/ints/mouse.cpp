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


#include <string.h>
#include <math.h>


#include "dosbox.h"
#include "callback.h"
#include "mem.h"
#include "regs.h"
#include "cpu.h"
#include "mouse.h"
#include "pic.h"
#include "inout.h"
#include "int10.h"
#include "bios.h"
#include "dos_inc.h"
#include "setup.h"

static Bitu call_int33,call_int74,int74_ret_callback,call_mouse_bd;
static Bit16u ps2cbseg,ps2cbofs;
static bool useps2callback,ps2callbackinit;
static Bitu call_ps2,call_uir;
static RealPt ps2_callback,uir_callback;
static Bit16s oldmouseX, oldmouseY;
// forward
void WriteMouseIntVector(void);
void MOUSE_Limit_Events(Bitu val);

struct button_event {
	Bit8u type;
	Bit8u buttons;
};

#define QUEUE_SIZE 32
#define MOUSE_BUTTONS 3
#define MOUSE_IRQ 12
#define POS_X (static_cast<Bit16s>(mouse.x) & mouse.gran_x)
#define POS_Y (static_cast<Bit16s>(mouse.y) & mouse.gran_y)

#define CURSORX 16
#define CURSORY 16
#define HIGHESTBIT (1<<(CURSORX-1))

static Bit16u defaultTextAndMask = 0x77FF;
static Bit16u defaultTextXorMask = 0x7700;

static Bit16u defaultScreenMask[CURSORY] = {
		0x3FFF, 0x1FFF, 0x0FFF, 0x07FF,
		0x03FF, 0x01FF, 0x00FF, 0x007F,
		0x003F, 0x001F, 0x01FF, 0x00FF,
		0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static Bit16u defaultCursorMask[CURSORY] = {
		0x0000, 0x4000, 0x6000, 0x7000,
		0x7800, 0x7C00, 0x7E00, 0x7F00,
		0x7F80, 0x7C00, 0x6C00, 0x4600,
		0x0600, 0x0300, 0x0300, 0x0000
};

static Bit16u userdefScreenMask[CURSORY];
static Bit16u userdefCursorMask[CURSORY];

static struct {
	Bit8u buttons;
	Bit16u times_pressed[MOUSE_BUTTONS];
	Bit16u times_released[MOUSE_BUTTONS];
	Bit16u last_released_x[MOUSE_BUTTONS];
	Bit16u last_released_y[MOUSE_BUTTONS];
	Bit16u last_pressed_x[MOUSE_BUTTONS];
	Bit16u last_pressed_y[MOUSE_BUTTONS];
	Bit16u hidden;
	float add_x,add_y;
	Bit16s min_x,max_x,min_y,max_y;
	float mickey_x,mickey_y;
	float x,y;
	button_event event_queue[QUEUE_SIZE];
	Bit8u events;//Increase if QUEUE_SIZE >255 (currently 32)
	Bit16u sub_seg,sub_ofs;
	Bit16u sub_mask;

	bool	background;
	Bit16s	backposx, backposy;
	Bit8u	backData[CURSORX*CURSORY];
	Bit16u*	screenMask;
	Bit16u* cursorMask;
	Bit16s	clipx,clipy;
	Bit16s  hotx,hoty;
	Bit16u  textAndMask, textXorMask;

	float	mickeysPerPixel_x;
	float	mickeysPerPixel_y;
	float	pixelPerMickey_x;
	float	pixelPerMickey_y;
	Bit16u	senv_x_val;
	Bit16u	senv_y_val;
	Bit16u	dspeed_val;
	float	senv_x;
	float	senv_y;
	Bit16s  updateRegion_x[2];
	Bit16s  updateRegion_y[2];
	Bit16u  doubleSpeedThreshold;
	Bit16u  language;
	Bit16u  cursorType;
	Bit16u	oldhidden;
	Bit8u  page;
	bool enabled;
	bool inhibit_draw;
	bool timer_in_progress;
	bool in_UIR;
	Bit8u mode;
	Bit16s gran_x,gran_y;
} mouse;

/* State maintained by the INT 15h/C2h pointing-device BIOS interface.
 * Keep it independent from the DOS INT 33h mouse state: Windows 3.x/9x
 * installs a BIOS PS/2 callback and expects reset, sampling, resolution and
 * scaling commands to have persistent, queryable semantics. */
struct PS2BiosState {
	bool initialized;
	Bit8u packet_size;
	Bit8u sample_rate_code;
	Bit8u resolution;
	Bit8u device_id;
	Bit8u unlock_index;
	bool scaling_21;
};

static PS2BiosState ps2_bios;
static const Bit8u ps2_sample_rates[7] = { 10, 20, 40, 60, 80, 100, 200 };

/* DOSBox-X integration I/O device (default ports 28h-2Bh).
 * Both the Windows 3.1 and Windows 95 DOSBox-X pointer drivers use this
 * interface.  Their normal BIOS PS/2 callback remains the notification and
 * button channel; this device supplies the host pointer's absolute position. */
enum MouseIntegrationMode {
	MOUSE_INTEGRATION_NONE = 0,
	MOUSE_INTEGRATION_WIN31,
	MOUSE_INTEGRATION_WIN95
};

enum MouseIntegrationPointerMode {
	MOUSE_POINTER_WINDOWS_ONLY = 0,
	MOUSE_POINTER_BOTH,
	MOUSE_POINTER_MORPHOS_ONLY
};

static const Bitu DOSBOX_ID_BASE = 0x28;
static const Bit32u DOSBOX_ID_RESET_DATA_CODE = 0x0d05b0c5UL;
static const Bit32u DOSBOX_ID_RESET_INDEX_CODE = 0xaa55bb66UL;
static const Bit32u DOSBOX_ID_IDENTIFICATION = 0xd05b0740UL;
static const Bit32u DOSBOX_ID_REG_IDENTIFY = 0x00000000UL;
static const Bit32u DOSBOX_ID_REG_VERSION_NUMBER = 0x00000003UL;
static const Bit32u DOSBOX_ID_REG_USER_MOUSE_STATUS = 0x00434d54UL;
static const Bit32u DOSBOX_ID_REG_USER_MOUSE_CURSOR = 0x00434d55UL;
static const Bit32u DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED = 0x00434d56UL;
static const Bit32u DOSBOX_ID_REG_RELEASE_MOUSE_CAPTURE = 0x0052434dUL;
static const Bit32u DOSBOX_ID_REG_GET_WINDOW_SIZE = 0x006845ffUL;

enum DosboxIdCommand {
	DOSBOX_ID_CMD_RESET_LATCH = 0x00,
	DOSBOX_ID_CMD_FLUSH_WRITE = 0x01,
	DOSBOX_ID_CMD_PUSH_STATE = 0x20,
	DOSBOX_ID_CMD_POP_STATE = 0x21,
	DOSBOX_ID_CMD_DISCARD_STATE = 0x22,
	DOSBOX_ID_CMD_DISCARD_ALL_STATE = 0x23,
	DOSBOX_ID_CMD_CLEAR_ERROR = 0xfe,
	DOSBOX_ID_CMD_RESET_INTERFACE = 0xff
};

struct DosboxIdSavedState {
	Bit32u reg_select;
	Bit32u data_latch;
	Bit8u reg_byte;
	Bit8u data_byte;
	bool error;
	bool reset_signature;
};

static struct {
	MouseIntegrationMode mode;
	MouseIntegrationPointerMode pointer_mode;
	bool guest_active;
	bool locked;
	Bit16u host_x;
	Bit16u host_y;
	Bit16u host_width;
	Bit16u host_height;
	Bit16u normalized_x;
	Bit16u normalized_y;

	Bit32u reg_select;
	Bit32u data_latch;
	Bit8u reg_byte;
	Bit8u data_byte;
	bool error;
	bool reset_signature;

	DosboxIdSavedState saved[8];
	Bit8u saved_count;
} mouse_integration;

static void Mouse_IntegrationNotifyFrontend(void)
{
#if defined(__MORPHOS__)
	Mouse_IntegrationStateChanged();
#endif
}

static void DOSBOX_ID_ResetLatch(void)
{
	mouse_integration.reg_byte = 0;
	mouse_integration.data_byte = 0;
}

static void DOSBOX_ID_ResetInterface(void)
{
	mouse_integration.reg_select = DOSBOX_ID_RESET_INDEX_CODE;
	mouse_integration.data_latch = DOSBOX_ID_RESET_DATA_CODE;
	mouse_integration.reg_byte = 0;
	mouse_integration.data_byte = 0;
	mouse_integration.error = false;
	mouse_integration.reset_signature = true;
	mouse_integration.saved_count = 0;
}

static Bit32u DOSBOX_ID_ReadRegister(Bit32u reg)
{
	switch (reg) {
	case DOSBOX_ID_REG_IDENTIFY:
		return DOSBOX_ID_IDENTIFICATION;
	case DOSBOX_ID_REG_VERSION_NUMBER:
		return 0x00010000UL;
	case DOSBOX_ID_REG_USER_MOUSE_STATUS:
		/* Bit 0 is the capture lock checked by the DOSBox-X pointer driver;
		 * bit 1 mirrors actual host capture as in DOSBox-X.  MorphOS uses one
		 * state for both, so report both bits together. */
		return mouse_integration.locked ? 3UL : 0UL;
	case DOSBOX_ID_REG_USER_MOUSE_CURSOR:
		return ((Bit32u)mouse_integration.host_y << 16) |
		       mouse_integration.host_x;
	case DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED:
		return ((Bit32u)mouse_integration.normalized_y << 16) |
		       mouse_integration.normalized_x;
	case DOSBOX_ID_REG_RELEASE_MOUSE_CAPTURE:
		return mouse_integration.locked ? 1UL : 0UL;
	case DOSBOX_ID_REG_GET_WINDOW_SIZE:
		return ((Bit32u)mouse_integration.host_height << 16) |
		       mouse_integration.host_width;
	default:
		return 0;
	}
}

static void DOSBOX_ID_WriteRegister(Bit32u reg, Bit32u value)
{
	switch (reg) {
	case DOSBOX_ID_REG_USER_MOUSE_CURSOR_NORMALIZED: {
		/* The DOSBox-X drivers write one after installing their BIOS PS/2
		 * callback and zero while shutting it down.  Force the MorphOS
		 * frontend to reevaluate capture/cursor visibility immediately. */
		const bool active = value != 0;
		if (mouse_integration.guest_active != active) {
			mouse_integration.guest_active = active;
			Mouse_IntegrationNotifyFrontend();
		}
		break;
	}
	case DOSBOX_ID_REG_RELEASE_MOUSE_CAPTURE:
		/* The MorphOS frontend owns capture state.  Reading this register is
		 * supported; a write is intentionally harmless rather than changing
		 * Intuition state from inside an emulated I/O instruction. */
		break;
	default:
		break;
	}
}

static Bitu DOSBOX_ID_Read(Bitu port, Bitu iolen)
{
	const Bitu offset = port - DOSBOX_ID_BASE;
	if (iolen != 1 && iolen != 2 && iolen != 4) return ~((Bitu)0);

	if (offset == 2) {
		return (mouse_integration.error ? 0x02 : 0) |
		       ((mouse_integration.data_byte & 3) << 4) |
		       ((mouse_integration.reg_byte & 3) << 6);
	}
	if (offset > 1) return ~((Bitu)0);

	Bitu result = 0;
	for (Bitu byte = 0; byte < iolen; ++byte) {
		if (offset == 0) {
			result |= (Bitu)((mouse_integration.reg_select >>
			                   (mouse_integration.reg_byte * 8)) & 0xffUL) <<
			          (byte * 8);
			mouse_integration.reg_byte =
				(Bit8u)((mouse_integration.reg_byte + 1) & 3);
		} else {
			if (mouse_integration.data_byte == 0) {
				mouse_integration.data_latch = mouse_integration.reset_signature ?
					DOSBOX_ID_RESET_DATA_CODE :
					DOSBOX_ID_ReadRegister(mouse_integration.reg_select);
			}
			result |= (Bitu)((mouse_integration.data_latch >>
			                   (mouse_integration.data_byte * 8)) & 0xffUL) <<
			          (byte * 8);
			mouse_integration.data_byte =
				(Bit8u)((mouse_integration.data_byte + 1) & 3);
		}
	}
	return result;
}

static void DOSBOX_ID_PushState(void)
{
	if (mouse_integration.saved_count >= 8) {
		mouse_integration.error = true;
		return;
	}
	DosboxIdSavedState &saved =
		mouse_integration.saved[mouse_integration.saved_count++];
	saved.reg_select = mouse_integration.reg_select;
	saved.data_latch = mouse_integration.data_latch;
	saved.reg_byte = mouse_integration.reg_byte;
	saved.data_byte = mouse_integration.data_byte;
	saved.error = mouse_integration.error;
	saved.reset_signature = mouse_integration.reset_signature;
}

static void DOSBOX_ID_PopState(bool restore)
{
	if (!mouse_integration.saved_count) {
		mouse_integration.error = true;
		return;
	}
	const DosboxIdSavedState saved =
		mouse_integration.saved[--mouse_integration.saved_count];
	if (!restore) return;
	mouse_integration.reg_select = saved.reg_select;
	mouse_integration.data_latch = saved.data_latch;
	mouse_integration.reg_byte = saved.reg_byte;
	mouse_integration.data_byte = saved.data_byte;
	mouse_integration.error = saved.error;
	mouse_integration.reset_signature = saved.reset_signature;
}

static void DOSBOX_ID_Command(Bit8u command)
{
	switch (command) {
	case DOSBOX_ID_CMD_RESET_LATCH:
		DOSBOX_ID_ResetLatch();
		break;
	case DOSBOX_ID_CMD_FLUSH_WRITE:
		if (mouse_integration.data_byte) {
			DOSBOX_ID_WriteRegister(mouse_integration.reg_select,
			                        mouse_integration.data_latch);
			mouse_integration.data_byte = 0;
		}
		break;
	case DOSBOX_ID_CMD_PUSH_STATE: {
		const bool can_push = mouse_integration.saved_count < 8;
		DOSBOX_ID_PushState();
		if (can_push) {
			mouse_integration.error = false;
			mouse_integration.reg_select = DOSBOX_ID_RESET_INDEX_CODE;
			mouse_integration.data_latch = DOSBOX_ID_RESET_DATA_CODE;
			mouse_integration.reg_byte = 0;
			mouse_integration.data_byte = 0;
			mouse_integration.reset_signature = true;
		}
		break;
	}
	case DOSBOX_ID_CMD_POP_STATE:
		DOSBOX_ID_PopState(true);
		break;
	case DOSBOX_ID_CMD_DISCARD_STATE:
		DOSBOX_ID_PopState(false);
		break;
	case DOSBOX_ID_CMD_DISCARD_ALL_STATE:
		mouse_integration.saved_count = 0;
		break;
	case DOSBOX_ID_CMD_CLEAR_ERROR:
		mouse_integration.error = false;
		break;
	case DOSBOX_ID_CMD_RESET_INTERFACE:
		DOSBOX_ID_ResetInterface();
		break;
	default:
		mouse_integration.error = true;
		break;
	}
}

static void DOSBOX_ID_Write(Bitu port, Bitu value, Bitu iolen)
{
	const Bitu offset = port - DOSBOX_ID_BASE;
	if (iolen != 1 && iolen != 2 && iolen != 4) return;
	if (offset == 2) {
		DOSBOX_ID_Command((Bit8u)value);
		return;
	}
	if (offset > 1) return;

	for (Bitu byte = 0; byte < iolen; ++byte) {
		const Bit32u input = ((Bit32u)value >> (byte * 8)) & 0xffUL;
		if (offset == 0) {
			const Bitu shift = mouse_integration.reg_byte * 8;
			mouse_integration.reg_select =
				(mouse_integration.reg_select & ~(0xffUL << shift)) |
				(input << shift);
			mouse_integration.reg_byte =
				(Bit8u)((mouse_integration.reg_byte + 1) & 3);
			if (mouse_integration.reg_byte == 0)
				mouse_integration.reset_signature = false;
		} else {
			const Bitu shift = mouse_integration.data_byte * 8;
			mouse_integration.data_latch =
				(mouse_integration.data_latch & ~(0xffUL << shift)) |
				(input << shift);
			mouse_integration.data_byte =
				(Bit8u)((mouse_integration.data_byte + 1) & 3);
			if (mouse_integration.data_byte == 0)
				DOSBOX_ID_WriteRegister(mouse_integration.reg_select,
				                        mouse_integration.data_latch);
		}
	}
}

bool Mouse_IntegrationConfigured(void)
{
	return mouse_integration.mode != MOUSE_INTEGRATION_NONE;
}

bool Mouse_IntegrationGuestActive(void)
{
	/* The guest-side register only requests integration.  Do not hide the
	 * MorphOS cursor or inject notification IRQs until the BIOS callback is
	 * installed and enabled as well.  This prevents the short C207h/C200h
	 * initialization window from being treated as an active driver. */
	return Mouse_IntegrationConfigured() && mouse_integration.guest_active &&
	       ps2callbackinit && useps2callback;
}

bool Mouse_IntegrationShowHostCursor(void)
{
	return mouse_integration.pointer_mode != MOUSE_POINTER_WINDOWS_ONLY;
}

bool Mouse_IntegrationShowGuestCursor(void)
{
	/* A manually captured pointer is invisible on the host.  Fall back to the
	 * guest cursor in that state so MorphOS-only mode can never leave the user
	 * without any visible pointer. */
	return mouse_integration.pointer_mode != MOUSE_POINTER_MORPHOS_ONLY ||
	       mouse_integration.locked;
}

void Mouse_IntegrationSetLocked(bool locked)
{
	mouse_integration.locked = locked;
}

static void Mouse_PS2ClearPendingEvents(void)
{
	PIC_RemoveEvents(MOUSE_Limit_Events);
	PIC_DeActivateIRQ(MOUSE_IRQ);
	mouse.timer_in_progress = false;
	mouse.in_UIR = false;
	mouse.events = 0;
	memset(mouse.event_queue,0,sizeof(mouse.event_queue));
	oldmouseX = static_cast<Bit16s>(mouse.x);
	oldmouseY = static_cast<Bit16s>(mouse.y);
}

static void Mouse_PS2ApplyDefaults(bool preserve_packet_size)
{
	const Bit8u packet_size = preserve_packet_size && ps2_bios.packet_size ?
	                         ps2_bios.packet_size : 3;
	ps2_bios.initialized = true;
	ps2_bios.packet_size = packet_size;
	ps2_bios.sample_rate_code = 5; /* 100 reports/second */
	ps2_bios.resolution = 2;       /* 4 counts/mm */
	ps2_bios.device_id = 0;        /* standard PS/2 pointing device */
	ps2_bios.unlock_index = 0;
	ps2_bios.scaling_21 = false;
}

bool Mouse_PS2CallbackInstalled(void)
{
	/* ES:BX=0000:0000 removes the callback.  Either component may legally
	 * be zero on its own, so do not reject a valid paragraph boundary. */
	return ps2callbackinit && (ps2cbseg != 0 || ps2cbofs != 0);
}

void Mouse_PS2Reset(void)
{
	/* IBM's C201h reset preserves the selected data package size but resets
	 * all other device characteristics and leaves the device disabled. */
	useps2callback = false;
	PIC_SetIRQMask(MOUSE_IRQ,true);
	Mouse_PS2ClearPendingEvents();
	Mouse_PS2ApplyDefaults(true);
	if (Mouse_IntegrationConfigured()) Mouse_IntegrationNotifyFrontend();
	else Mouse_AutoLock(false);
}

bool Mouse_PS2Initialize(Bit8u packet_size)
{
	/* DOSBox-X supports the standard three-byte packet and a four-byte
	 * IntelliMouse packet.  The Win3.1/Win95 integration drivers request 3. */
	if (packet_size != 3 && !(packet_size == 4 && ps2_bios.device_id == 3))
		return false;

	useps2callback = false;
	PIC_SetIRQMask(MOUSE_IRQ,true);
	Mouse_PS2ClearPendingEvents();
	Mouse_PS2ApplyDefaults(false);
	ps2_bios.packet_size = packet_size;
	if (Mouse_IntegrationConfigured()) Mouse_IntegrationNotifyFrontend();
	else Mouse_AutoLock(false);
	return true;
}

bool Mouse_PS2SetSamplingRate(Bit8u rate_code)
{
	if (rate_code >= 7) return false;
	ps2_bios.sample_rate_code = rate_code;

	/* Standard IntelliMouse unlock sequence: 200, 100, 80 reports/s. */
	static const Bit8u unlock_sequence[3] = { 6, 5, 4 };
	if (rate_code == unlock_sequence[ps2_bios.unlock_index]) {
		if (++ps2_bios.unlock_index == 3) {
			ps2_bios.device_id = 3;
			ps2_bios.unlock_index = 0;
		}
	} else {
		ps2_bios.unlock_index = (rate_code == unlock_sequence[0]) ? 1 : 0;
	}
	return true;
}

bool Mouse_PS2SetResolution(Bit8u resolution)
{
	if (resolution > 3) return false;
	ps2_bios.resolution = resolution;
	return true;
}

void Mouse_PS2SetScaling(bool scaling_21)
{
	ps2_bios.scaling_21 = scaling_21;
}

Bit8u Mouse_PS2GetType(void)
{
	return ps2_bios.device_id;
}

void Mouse_PS2GetStatus(Bit8u &status, Bit8u &resolution, Bit8u &sample_rate)
{
	/* IBM status layout: bit 0 right, bit 2 left, bit 4 2:1 scaling,
	 * bit 5 enabled, bit 6 remote mode (we always operate in stream mode). */
	status = 0;
	if (mouse.buttons & 0x02) status |= 0x01;
	if (mouse.buttons & 0x01) status |= 0x04;
	if (ps2_bios.scaling_21) status |= 0x10;
	if (useps2callback) status |= 0x20;
	resolution = ps2_bios.resolution;
	sample_rate = ps2_sample_rates[ps2_bios.sample_rate_code];
}

bool Mouse_SetPS2State(bool use) {
	if (use) {
		if (!Mouse_PS2CallbackInstalled()) {
			useps2callback = false;
			PIC_SetIRQMask(MOUSE_IRQ,true);
			return false;
		}
		if (!ps2_bios.initialized) Mouse_PS2ApplyDefaults(false);
		Mouse_PS2ClearPendingEvents();
		useps2callback = true;
		PIC_SetIRQMask(MOUSE_IRQ,false);
		/* Absolute integration must never request MorphOS pointer capture. */
		if (Mouse_IntegrationConfigured()) Mouse_IntegrationNotifyFrontend();
		else Mouse_AutoLock(true);
	} else {
		useps2callback = false;
		Mouse_PS2ClearPendingEvents();
		PIC_SetIRQMask(MOUSE_IRQ,true);
		if (Mouse_IntegrationConfigured()) Mouse_IntegrationNotifyFrontend();
		else Mouse_AutoLock(false);
	}
	return true;
}

void Mouse_ChangePS2Callback(Bit16u pseg, Bit16u pofs) {
	if ((pseg==0) && (pofs==0)) {
		Mouse_SetPS2State(false);
		ps2callbackinit = false;
		ps2cbseg = 0;
		ps2cbofs = 0;
	} else {
		ps2callbackinit = true;
		ps2cbseg = pseg;
		ps2cbofs = pofs;
		oldmouseX = static_cast<Bit16s>(mouse.x);
		oldmouseY = static_cast<Bit16s>(mouse.y);
	}
	if (Mouse_IntegrationConfigured()) Mouse_IntegrationNotifyFrontend();
	else Mouse_AutoLock(ps2callbackinit);
}

void DoPS2Callback(Bit16u data, Bit16s mouseX, Bit16s mouseY) {
	if (useps2callback && Mouse_PS2CallbackInstalled()) {
		Bit16u mdat = (data & 0x03) | 0x08;
		Bit16s xdiff = mouseX-oldmouseX;
		Bit16s ydiff = oldmouseY-mouseY;
		oldmouseX = mouseX;
		oldmouseY = mouseY;
		if ((xdiff>0xff) || (xdiff<-0xff)) mdat |= 0x40;
		if ((ydiff>0xff) || (ydiff<-0xff)) mdat |= 0x80;
		xdiff %= 256;
		ydiff %= 256;
		if (xdiff<0) {
			xdiff = (0x100+xdiff);
			mdat |= 0x10;
		}
		if (ydiff<0) {
			ydiff = (0x100+ydiff);
			mdat |= 0x20;
		}
		CPU_Push16((Bit16u)mdat);
		CPU_Push16((Bit16u)(xdiff % 256));
		CPU_Push16((Bit16u)(ydiff % 256));
		/* The BIOS callback ABI always supplies a fourth data word.  Standard
		 * devices report zero; this also gives the Win9x driver a stable value. */
		CPU_Push16((Bit16u)0);
		CPU_Push16(RealSeg(ps2_callback));
		CPU_Push16(RealOff(ps2_callback));
		SegSet16(cs, ps2cbseg);
		reg_ip = ps2cbofs;
	}
}

Bitu PS2_Handler(void) {
	CPU_Pop16();CPU_Pop16();CPU_Pop16();CPU_Pop16();
	return CBRET_NONE;
}


#define X_MICKEY 8
#define Y_MICKEY 8

#define MOUSE_HAS_MOVED 1
#define MOUSE_LEFT_PRESSED 2
#define MOUSE_LEFT_RELEASED 4
#define MOUSE_RIGHT_PRESSED 8
#define MOUSE_RIGHT_RELEASED 16
#define MOUSE_MIDDLE_PRESSED 32
#define MOUSE_MIDDLE_RELEASED 64
#define MOUSE_DELAY 5.0

void MOUSE_Limit_Events(Bitu /*val*/) {
	mouse.timer_in_progress = false;
	if (mouse.events) {
		mouse.timer_in_progress = true;
		PIC_AddEvent(MOUSE_Limit_Events,MOUSE_DELAY);
		PIC_ActivateIRQ(MOUSE_IRQ);
	}
}

INLINE void Mouse_AddEvent(Bit8u type) {
	if (mouse.events<QUEUE_SIZE) {
		if (mouse.events>0) {
			/* Skip duplicate events */
			if (type==MOUSE_HAS_MOVED) return;
			/* Always put the newest element in the front as that the events are 
			 * handled backwards (prevents doubleclicks while moving)
			 */
			for(Bitu i = mouse.events ; i ; i--)
				mouse.event_queue[i] = mouse.event_queue[i-1];
		}
		mouse.event_queue[0].type=type;
		mouse.event_queue[0].buttons=mouse.buttons;
		mouse.events++;
	}
	if (!mouse.timer_in_progress) {
		mouse.timer_in_progress = true;
		PIC_AddEvent(MOUSE_Limit_Events,MOUSE_DELAY);
		PIC_ActivateIRQ(MOUSE_IRQ);
	}
}

void Mouse_IntegrationHostPosition(Bit16u x, Bit16u y,
                                   Bit16u width, Bit16u height)
{
	if (!Mouse_IntegrationConfigured()) return;

	if (!width) width = 1;
	if (!height) height = 1;
	if (x >= width) x = width - 1;
	if (y >= height) y = height - 1;

	Bit32u normalized_x32 = width > 1 ?
		((Bit32u)x << 16) / (width - 1) : 0;
	Bit32u normalized_y32 = height > 1 ?
		((Bit32u)y << 16) / (height - 1) : 0;
	if (normalized_x32 > 0xffffUL) normalized_x32 = 0xffffUL;
	if (normalized_y32 > 0xffffUL) normalized_y32 = 0xffffUL;
	const Bit16u normalized_x = (Bit16u)normalized_x32;
	const Bit16u normalized_y = (Bit16u)normalized_y32;
	const bool changed = normalized_x != mouse_integration.normalized_x ||
	                     normalized_y != mouse_integration.normalized_y;

	mouse_integration.host_x = x;
	mouse_integration.host_y = y;
	mouse_integration.host_width = width;
	mouse_integration.host_height = height;
	mouse_integration.normalized_x = normalized_x;
	mouse_integration.normalized_y = normalized_y;

	/* The DOSBox-X driver reads the absolute coordinates from the I/O device,
	 * but it deliberately uses the normal BIOS PS/2 callback as notification. */
	if (changed && Mouse_IntegrationGuestActive() && !mouse_integration.locked)
		Mouse_AddEvent(MOUSE_HAS_MOVED);
}

// ***************************************************************************
// Mouse cursor - text mode
// ***************************************************************************
/* Write and read directly to the screen. Do no use int_setcursorpos (LOTUS123) */
extern void WriteChar(Bit16u col,Bit16u row,Bit8u page,Bit8u chr,Bit8u attr,bool useattr);
extern void ReadCharAttr(Bit16u col,Bit16u row,Bit8u page,Bit16u * result);

void RestoreCursorBackgroundText() {
	if (mouse.hidden || mouse.inhibit_draw) return;

	if (mouse.background) {
		WriteChar(mouse.backposx,mouse.backposy,real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE),mouse.backData[0],mouse.backData[1],true);
		mouse.background = false;
	}
}

void DrawCursorText() {	
	// Restore Background
	RestoreCursorBackgroundText();

	// Check if cursor in update region
	if ((POS_Y <= mouse.updateRegion_y[1]) && (POS_Y >= mouse.updateRegion_y[0]) &&
		(POS_X <= mouse.updateRegion_x[1]) && (POS_X >= mouse.updateRegion_x[0])) {
		return;
	}

	// Save Background
	mouse.backposx		= POS_X>>3;
	mouse.backposy		= POS_Y>>3;
	if (mouse.mode < 2) mouse.backposx >>= 1; 

	//use current page (CV program)
	Bit8u page = real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE);
	
	if (mouse.cursorType == 0) {
		Bit16u result;
		ReadCharAttr(mouse.backposx,mouse.backposy,page,&result);
		mouse.backData[0]	= (Bit8u)(result & 0xFF);
		mouse.backData[1]	= (Bit8u)(result>>8);
		mouse.background	= true;
		// Write Cursor
		result = (result & mouse.textAndMask) ^ mouse.textXorMask;
		WriteChar(mouse.backposx,mouse.backposy,page,(Bit8u)(result&0xFF),(Bit8u)(result>>8),true);
	} else {
		Bit16u address=page * real_readw(BIOSMEM_SEG,BIOSMEM_PAGE_SIZE);
		address += (mouse.backposy * real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS) + mouse.backposx) * 2;
		address /= 2;
		Bit16u cr = real_readw(BIOSMEM_SEG,BIOSMEM_CRTC_ADDRESS);
		IO_Write(cr    , 0xe);
		IO_Write(cr + 1, (address>>8) & 0xff);
		IO_Write(cr    , 0xf);
		IO_Write(cr + 1, address & 0xff);
	}
}

// ***************************************************************************
// Mouse cursor - graphic mode
// ***************************************************************************

static Bit8u gfxReg3CE[9];
static Bit8u index3C4,gfxReg3C5;
void SaveVgaRegisters() {
	if (IS_VGA_ARCH) {
		for (Bit8u i=0; i<9; i++) {
			IO_Write	(0x3CE,i);
			gfxReg3CE[i] = IO_Read(0x3CF);
		}
		/* Setup some default values in GFX regs that should work */
		IO_Write(0x3CE,3); IO_Write(0x3Cf,0);				//disable rotate and operation
		IO_Write(0x3CE,5); IO_Write(0x3Cf,gfxReg3CE[5]&0xf0);	//Force read/write mode 0

		//Set Map to all planes. Celtic Tales
		index3C4 = IO_Read(0x3c4);  IO_Write(0x3C4,2);
		gfxReg3C5 = IO_Read(0x3C5); IO_Write(0x3C5,0xF);
	} else if (machine==MCH_EGA) {
		//Set Map to all planes.
		IO_Write(0x3C4,2);
		IO_Write(0x3C5,0xF);
	}
}

void RestoreVgaRegisters() {
	if (IS_VGA_ARCH) {
		for (Bit8u i=0; i<9; i++) {
			IO_Write(0x3CE,i);
			IO_Write(0x3CF,gfxReg3CE[i]);
		}

		IO_Write(0x3C4,2);
		IO_Write(0x3C5,gfxReg3C5);
		IO_Write(0x3C4,index3C4);
	}
}

void ClipCursorArea(Bit16s& x1, Bit16s& x2, Bit16s& y1, Bit16s& y2,
					Bit16u& addx1, Bit16u& addx2, Bit16u& addy) {
	addx1 = addx2 = addy = 0;
	// Clip up
	if (y1<0) {
		addy += (-y1);
		y1 = 0;
	}
	// Clip down
	if (y2>mouse.clipy) {
		y2 = mouse.clipy;		
	};
	// Clip left
	if (x1<0) {
		addx1 += (-x1);
		x1 = 0;
	};
	// Clip right
	if (x2>mouse.clipx) {
		addx2 = x2 - mouse.clipx;
		x2 = mouse.clipx;
	};
}

void RestoreCursorBackground() {
	if (mouse.hidden || mouse.inhibit_draw) return;

	SaveVgaRegisters();
	if (mouse.background) {
		// Restore background
		Bit16s x,y;
		Bit16u addx1,addx2,addy;
		Bit16u dataPos	= 0;
		Bit16s x1		= mouse.backposx;
		Bit16s y1		= mouse.backposy;
		Bit16s x2		= x1 + CURSORX - 1;
		Bit16s y2		= y1 + CURSORY - 1;	

		ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

		dataPos = addy * CURSORX;
		for (y=y1; y<=y2; y++) {
			dataPos += addx1;
			for (x=x1; x<=x2; x++) {
				INT10_PutPixel(x,y,mouse.page,mouse.backData[dataPos++]);
			};
			dataPos += addx2;
		};
		mouse.background = false;
	};
	RestoreVgaRegisters();
}

void DrawCursor() {
	if (mouse.hidden || mouse.inhibit_draw) return;
	INT10_SetCurMode();
	// In Textmode ?
	if (CurMode->type==M_TEXT) {
		DrawCursorText();
		return;
	}

	// Check video page. Seems to be ignored for text mode. 
	// hence the text mode handled above this
	// >>> removed because BIOS page is not actual page in some cases, e.g. QQP games
//	if (real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE)!=mouse.page) return;

// Check if cursor in update region
/*	if ((POS_X >= mouse.updateRegion_x[0]) && (POS_X <= mouse.updateRegion_x[1]) &&
	    (POS_Y >= mouse.updateRegion_y[0]) && (POS_Y <= mouse.updateRegion_y[1])) {
		if (CurMode->type==M_TEXT16)
			RestoreCursorBackgroundText();
		else
			RestoreCursorBackground();
		mouse.shown--;
		return;
	}
   */ /*Not sure yet what to do update region should be set to ??? */
		 
	// Get Clipping ranges


	mouse.clipx = (Bit16s)((Bits)CurMode->swidth-1);	/* Get from bios ? */
	mouse.clipy = (Bit16s)((Bits)CurMode->sheight-1);

	/* might be vidmode == 0x13?2:1 */
	Bit16s xratio = 640;
	if (CurMode->swidth>0) xratio/=CurMode->swidth;
	if (xratio==0) xratio = 1;
	
	RestoreCursorBackground();

	SaveVgaRegisters();

	// Save Background
	Bit16s x,y;
	Bit16u addx1,addx2,addy;
	Bit16u dataPos	= 0;
	Bit16s x1		= POS_X / xratio - mouse.hotx;
	Bit16s y1		= POS_Y - mouse.hoty;
	Bit16s x2		= x1 + CURSORX - 1;
	Bit16s y2		= y1 + CURSORY - 1;	

	ClipCursorArea(x1,x2,y1,y2, addx1, addx2, addy);

	dataPos = addy * CURSORX;
	for (y=y1; y<=y2; y++) {
		dataPos += addx1;
		for (x=x1; x<=x2; x++) {
			INT10_GetPixel(x,y,mouse.page,&mouse.backData[dataPos++]);
		};
		dataPos += addx2;
	};
	mouse.background= true;
	mouse.backposx	= POS_X / xratio - mouse.hotx;
	mouse.backposy	= POS_Y - mouse.hoty;

	// Draw Mousecursor
	dataPos = addy * CURSORX;
	for (y=y1; y<=y2; y++) {
		Bit16u scMask = mouse.screenMask[addy+y-y1];
		Bit16u cuMask = mouse.cursorMask[addy+y-y1];
		if (addx1>0) { scMask<<=addx1; cuMask<<=addx1; dataPos += addx1; };
		for (x=x1; x<=x2; x++) {
			Bit8u pixel = 0;
			// ScreenMask
			if (scMask & HIGHESTBIT) pixel = mouse.backData[dataPos];
			scMask<<=1;
			// CursorMask
			if (cuMask & HIGHESTBIT) pixel = pixel ^ 0x0F;
			cuMask<<=1;
			// Set Pixel
			INT10_PutPixel(x,y,mouse.page,pixel);
			dataPos++;
		};
		dataPos += addx2;
	};
	RestoreVgaRegisters();
}

void Mouse_CursorMoved(float xrel,float yrel,float x,float y,bool emulate) {
	float dx = xrel * mouse.pixelPerMickey_x;
	float dy = yrel * mouse.pixelPerMickey_y;

	if((fabs(xrel) > 1.0) || (mouse.senv_x < 1.0)) dx *= mouse.senv_x;
	if((fabs(yrel) > 1.0) || (mouse.senv_y < 1.0)) dy *= mouse.senv_y;
#if !defined(__MORPHOS__)
	/* Legacy DOSBox behaviour doubles PS/2 Y movement.  MorphOS already
	 * supplies host-processed deltas, and Windows 95 applies its own PS/2
	 * acceleration, so doubling here makes the guest cursor outrun the host. */
	if (useps2callback) dy *= 2;
#endif

	mouse.mickey_x += (dx * mouse.mickeysPerPixel_x);
	mouse.mickey_y += (dy * mouse.mickeysPerPixel_y);
	if (mouse.mickey_x >= 32768.0) mouse.mickey_x -= 65536.0;
	else if (mouse.mickey_x <= -32769.0) mouse.mickey_x += 65536.0;
	if (mouse.mickey_y >= 32768.0) mouse.mickey_y -= 65536.0;
	else if (mouse.mickey_y <= -32769.0) mouse.mickey_y += 65536.0;
	if (emulate) {
		mouse.x += dx;
		mouse.y += dy;
	} else {
		if (CurMode->type == M_TEXT) {
			mouse.x = x*real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS)*8;
			mouse.y = y*(real_readb(BIOSMEM_SEG,BIOSMEM_NB_ROWS)+1)*8;
		} else if ((mouse.max_x < 2048) || (mouse.max_y < 2048) || (mouse.max_x != mouse.max_y)) {
			if ((mouse.max_x > 0) && (mouse.max_y > 0)) {
				mouse.x = x*mouse.max_x;
				mouse.y = y*mouse.max_y;
			} else {
				mouse.x += xrel;
				mouse.y += yrel;
			}
		} else { // Games faking relative movement through absolute coordinates. Quite surprising that this actually works..
			mouse.x += xrel;
			mouse.y += yrel;
		}
	}

	/* ignore constraints if using PS2 mouse callback in the bios */

	if (!useps2callback) {		
		if (mouse.x > mouse.max_x) mouse.x = mouse.max_x;
		if (mouse.x < mouse.min_x) mouse.x = mouse.min_x;
		if (mouse.y > mouse.max_y) mouse.y = mouse.max_y;
		if (mouse.y < mouse.min_y) mouse.y = mouse.min_y;
	} else {
		if (mouse.x >= 32768.0) mouse.x -= 65536.0;
		else if (mouse.x <= -32769.0) mouse.x += 65536.0;
		if (mouse.y >= 32768.0) mouse.y -= 65536.0;
		else if (mouse.y <= -32769.0) mouse.y += 65536.0;
	}
	Mouse_AddEvent(MOUSE_HAS_MOVED);
	DrawCursor();
}

void Mouse_CursorSet(float x,float y) {
	mouse.x=x;
	mouse.y=y;
	DrawCursor();
}

void Mouse_ButtonPressed(Bit8u button) {
	switch (button) {
#if (MOUSE_BUTTONS >= 1)
	case 0:
		if (mouse.buttons&1) return;
		mouse.buttons|=1;
		Mouse_AddEvent(MOUSE_LEFT_PRESSED);
		break;
#endif
#if (MOUSE_BUTTONS >= 2)
	case 1:
		if (mouse.buttons&2) return;
		mouse.buttons|=2;
		Mouse_AddEvent(MOUSE_RIGHT_PRESSED);
		break;
#endif
#if (MOUSE_BUTTONS >= 3)
	case 2:
		if (mouse.buttons&4) return;
		mouse.buttons|=4;
		Mouse_AddEvent(MOUSE_MIDDLE_PRESSED);
		break;
#endif
	default:
		return;
	}
	mouse.times_pressed[button]++;
	mouse.last_pressed_x[button]=POS_X;
	mouse.last_pressed_y[button]=POS_Y;
}

void Mouse_ButtonReleased(Bit8u button) {
	switch (button) {
#if (MOUSE_BUTTONS >= 1)
	case 0:
		if (!(mouse.buttons&1)) return;
		mouse.buttons&=~1;
		Mouse_AddEvent(MOUSE_LEFT_RELEASED);
		break;
#endif
#if (MOUSE_BUTTONS >= 2)
	case 1:
		if (!(mouse.buttons&2)) return;
		mouse.buttons&=~2;
		Mouse_AddEvent(MOUSE_RIGHT_RELEASED);
		break;
#endif
#if (MOUSE_BUTTONS >= 3)
	case 2:
		if (!(mouse.buttons&4)) return;
		mouse.buttons&=~4;
		Mouse_AddEvent(MOUSE_MIDDLE_RELEASED);
		break;
#endif
	default:
		return;
	}
	mouse.times_released[button]++;	
	mouse.last_released_x[button]=POS_X;
	mouse.last_released_y[button]=POS_Y;
}

static void Mouse_SetMickeyPixelRate(Bit16s px, Bit16s py){
	if ((px!=0) && (py!=0)) {
		mouse.mickeysPerPixel_x	 = (float)px/X_MICKEY;
		mouse.mickeysPerPixel_y  = (float)py/Y_MICKEY;
		mouse.pixelPerMickey_x	 = X_MICKEY/(float)px;
		mouse.pixelPerMickey_y 	 = Y_MICKEY/(float)py;	
	}
}

static void Mouse_SetSensitivity(Bit16u px, Bit16u py, Bit16u dspeed){
	if(px>100) px=100;
	if(py>100) py=100;
	if(dspeed>100) dspeed=100;
	// save values
	mouse.senv_x_val=px;
	mouse.senv_y_val=py;
	mouse.dspeed_val=dspeed;
	if ((px!=0) && (py!=0)) {
		px--;  //Inspired by cutemouse 
		py--;  //Although their cursor update routine is far more complex then ours
		mouse.senv_x=(static_cast<float>(px)*px)/3600.0f +1.0f/3.0f;
		mouse.senv_y=(static_cast<float>(py)*py)/3600.0f +1.0f/3.0f;
     }
}


static void Mouse_ResetHardware(void){
	PIC_SetIRQMask(MOUSE_IRQ,false);
}

void Mouse_BeforeNewVideoMode(bool setmode) {
	if (CurMode->type!=M_TEXT) RestoreCursorBackground();
	else RestoreCursorBackgroundText();
	mouse.hidden = 1;
	mouse.oldhidden = 1;
	mouse.background = false;
}

//Does way to much. Many things should be moved to mouse reset one day
void Mouse_AfterNewVideoMode(bool setmode) {
	mouse.inhibit_draw = false;
	/* Get the correct resolution from the current video mode */
	Bit8u mode = mem_readb(BIOS_VIDEO_MODE);
	if (setmode && mode == mouse.mode) LOG(LOG_MOUSE,LOG_NORMAL)("New video mode is the same as the old");
	mouse.gran_x = (Bit16s)0xffff;
	mouse.gran_y = (Bit16s)0xffff;
	switch (mode) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x07: {
		mouse.gran_x = (mode<2)?0xfff0:0xfff8;
		mouse.gran_y = (Bit16s)0xfff8;
		Bitu rows = real_readb(BIOSMEM_SEG,BIOSMEM_NB_ROWS);
		if ((rows == 0) || (rows > 250)) rows = 25 - 1;
		mouse.max_y = 8*(rows+1) - 1;
		break;
	}
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0d:
	case 0x0e:
	case 0x13:
		if (mode == 0x0d || mode == 0x13) mouse.gran_x = (Bit16s)0xfffe;
		mouse.max_y = 199;
		break;
	case 0x0f:
	case 0x10:
		mouse.max_y = 349;
		break;
	case 0x11:
	case 0x12:
		mouse.max_y = 479;
		break;
	default:
		LOG(LOG_MOUSE,LOG_ERROR)("Unhandled videomode %X on reset",mode);
		mouse.inhibit_draw = true;
		return;
	}
	mouse.mode = mode;
	mouse.max_x = 639;
	mouse.min_x = 0;
	mouse.min_y = 0;

	mouse.events = 0;
	mouse.timer_in_progress = false;
	PIC_RemoveEvents(MOUSE_Limit_Events);

	mouse.hotx		 = 0;
	mouse.hoty		 = 0;
	mouse.screenMask = defaultScreenMask;
	mouse.cursorMask = defaultCursorMask;
	mouse.textAndMask= defaultTextAndMask;
	mouse.textXorMask= defaultTextXorMask;
	mouse.language   = 0;
	mouse.page               = 0;
	mouse.doubleSpeedThreshold = 64;
	mouse.updateRegion_y[1] = -1; //offscreen
	mouse.cursorType = 0; //Test
	mouse.enabled=true;

	oldmouseX = static_cast<Bit16s>(mouse.x);
	oldmouseY = static_cast<Bit16s>(mouse.y);


}

//Much too empty, Mouse_NewVideoMode contains stuff that should be in here
static void Mouse_Reset(void) {
	Mouse_BeforeNewVideoMode(false);
	Mouse_AfterNewVideoMode(false);
	Mouse_SetMickeyPixelRate(8,16);

	mouse.mickey_x = 0;
	mouse.mickey_y = 0;

	mouse.buttons = 0;

	for (Bit16u but=0; but<MOUSE_BUTTONS; but++) {
		mouse.times_pressed[but] = 0;
		mouse.times_released[but] = 0;
		mouse.last_pressed_x[but] = 0;
		mouse.last_pressed_y[but] = 0;
		mouse.last_released_x[but] = 0;
		mouse.last_released_y[but] = 0;
	}

	// Dont set max coordinates here. it is done by SetResolution!
	mouse.x = static_cast<float>((mouse.max_x + 1)/ 2);
	mouse.y = static_cast<float>((mouse.max_y + 1)/ 2);
	mouse.sub_mask = 0;
	mouse.in_UIR = false;
}

static void Mouse_Used(void) {
	static bool autolock_enabled=false;
	if (!autolock_enabled) {
		Mouse_AutoLock(true);
		autolock_enabled=true;
	}
}

static Bitu INT33_Handler(void) {
//	LOG(LOG_MOUSE,LOG_NORMAL)("MOUSE: %04X %X %X %d %d",reg_ax,reg_bx,reg_cx,POS_X,POS_Y);
	switch (reg_ax) {
	case 0x00:	/* Reset Driver and Read Status */
		Mouse_ResetHardware(); /* fallthrough */
	case 0x21:	/* Software Reset */
		reg_ax=0xffff;
		reg_bx=MOUSE_BUTTONS;
		Mouse_Reset();
		Mouse_Used();
		break;
	case 0x01:	/* Show Mouse */
		if(mouse.hidden) mouse.hidden--;
		mouse.updateRegion_y[1] = -1; //offscreen
		DrawCursor();
		if (!mouse.hidden) Mouse_Used();
		break;
	case 0x02:	/* Hide Mouse */
		{
			if (CurMode->type!=M_TEXT) RestoreCursorBackground();
			else RestoreCursorBackgroundText();
			mouse.hidden++;
		}
		break;
	case 0x03:	/* Return position and Button Status */
		reg_bx=mouse.buttons;
		reg_cx=POS_X;
		reg_dx=POS_Y;
		Mouse_Used();
		break;
	case 0x04:	/* Position Mouse */
		/* If position isn't different from current position
		 * don't change it then. (as position is rounded so numbers get
		 * lost when the rounded number is set) (arena/simulation Wolf) */
		if ((Bit16s)reg_cx >= mouse.max_x) mouse.x = static_cast<float>(mouse.max_x);
		else if (mouse.min_x >= (Bit16s)reg_cx) mouse.x = static_cast<float>(mouse.min_x); 
		else if ((Bit16s)reg_cx != POS_X) mouse.x = static_cast<float>(reg_cx);

		if ((Bit16s)reg_dx >= mouse.max_y) mouse.y = static_cast<float>(mouse.max_y);
		else if (mouse.min_y >= (Bit16s)reg_dx) mouse.y = static_cast<float>(mouse.min_y); 
		else if ((Bit16s)reg_dx != POS_Y) mouse.y = static_cast<float>(reg_dx);
		DrawCursor();
		break;
	case 0x05:	/* Return Button Press Data */
		{
			Bit16u but=reg_bx;
			reg_ax=mouse.buttons;
			if (but>=MOUSE_BUTTONS) but = MOUSE_BUTTONS - 1;
			reg_cx=mouse.last_pressed_x[but];
			reg_dx=mouse.last_pressed_y[but];
			reg_bx=mouse.times_pressed[but];
			mouse.times_pressed[but]=0;
		}
		Mouse_Used();
		break;
	case 0x06:	/* Return Button Release Data */
		{
			Bit16u but=reg_bx;
			reg_ax=mouse.buttons;
			if (but>=MOUSE_BUTTONS) but = MOUSE_BUTTONS - 1;
			reg_cx=mouse.last_released_x[but];
			reg_dx=mouse.last_released_y[but];
			reg_bx=mouse.times_released[but];
			mouse.times_released[but]=0;
		}
		Mouse_Used();
		break;
	case 0x07:	/* Define horizontal cursor range */
		{	//lemmings set 1-640 and wants that. iron seeds set 0-640 but doesn't like 640
			//Iron seed works if newvideo mode with mode 13 sets 0-639
			//Larry 6 actually wants newvideo mode with mode 13 to set it to 0-319
			Bit16s max,min;
			if ((Bit16s)reg_cx<(Bit16s)reg_dx) { min=(Bit16s)reg_cx;max=(Bit16s)reg_dx;}
			else { min=(Bit16s)reg_dx;max=(Bit16s)reg_cx;}
			mouse.min_x=min;
			mouse.max_x=max;
			/* Battlechess wants this */
			if(mouse.x > mouse.max_x) mouse.x = mouse.max_x;
			if(mouse.x < mouse.min_x) mouse.x = mouse.min_x;
			/* Or alternatively this: 
			mouse.x = (mouse.max_x - mouse.min_x + 1)/2;*/
			LOG(LOG_MOUSE,LOG_NORMAL)("Define Hortizontal range min:%d max:%d",min,max);
		}
		break;
	case 0x08:	/* Define vertical cursor range */
		{	// not sure what to take instead of the CurMode (see case 0x07 as well)
			// especially the cases where sheight= 400 and we set it with the mouse_reset to 200
			//disabled it at the moment. Seems to break syndicate who want 400 in mode 13
			Bit16s max,min;
			if ((Bit16s)reg_cx<(Bit16s)reg_dx) { min=(Bit16s)reg_cx;max=(Bit16s)reg_dx;}
			else { min=(Bit16s)reg_dx;max=(Bit16s)reg_cx;}
			mouse.min_y=min;
			mouse.max_y=max;
			/* Battlechess wants this */
			if(mouse.y > mouse.max_y) mouse.y = mouse.max_y;
			if(mouse.y < mouse.min_y) mouse.y = mouse.min_y;
			/* Or alternatively this: 
			mouse.y = (mouse.max_y - mouse.min_y + 1)/2;*/
			LOG(LOG_MOUSE,LOG_NORMAL)("Define Vertical range min:%d max:%d",min,max);
		}
		break;
	case 0x09:	/* Define GFX Cursor */
		{
			PhysPt src = SegPhys(es)+reg_dx;
			MEM_BlockRead(src          ,userdefScreenMask,CURSORY*2);
			MEM_BlockRead(src+CURSORY*2,userdefCursorMask,CURSORY*2);
			mouse.screenMask = userdefScreenMask;
			mouse.cursorMask = userdefCursorMask;
			mouse.hotx		 = reg_bx;
			mouse.hoty		 = reg_cx;
			mouse.cursorType = 2;
			DrawCursor();
		}
		break;
	case 0x0a:	/* Define Text Cursor */
		mouse.cursorType = (reg_bx?1:0);
		mouse.textAndMask = reg_cx;
		mouse.textXorMask = reg_dx;
		if (reg_bx) {
			INT10_SetCursorShape(reg_cl,reg_dl);
			LOG(LOG_MOUSE,LOG_NORMAL)("Hardware Text cursor selected");
		}
		DrawCursor();
		break;
	case 0x0b:	/* Read Motion Data */
		reg_cx=static_cast<Bit16s>(mouse.mickey_x);
		reg_dx=static_cast<Bit16s>(mouse.mickey_y);
		mouse.mickey_x=0;
		mouse.mickey_y=0;
		Mouse_Used();
		break;
	case 0x0c:	/* Define interrupt subroutine parameters */
		mouse.sub_mask=reg_cx;
		mouse.sub_seg=SegValue(es);
		mouse.sub_ofs=reg_dx;
		if (mouse.sub_mask) Mouse_Used();
		break;
	case 0x0f:	/* Define mickey/pixel rate */
		Mouse_SetMickeyPixelRate(reg_cx,reg_dx);
		break;
	case 0x10:	/* Define screen region for updating */
		mouse.updateRegion_x[0]=(Bit16s)reg_cx;
		mouse.updateRegion_y[0]=(Bit16s)reg_dx;
		mouse.updateRegion_x[1]=(Bit16s)reg_si;
		mouse.updateRegion_y[1]=(Bit16s)reg_di;
		DrawCursor();
		break;
	case 0x11:      /* Get number of buttons */
		reg_ax=0xffff;
		reg_bx=MOUSE_BUTTONS;
		break;
	case 0x13:      /* Set double-speed threshold */
		mouse.doubleSpeedThreshold=(reg_bx ? reg_bx : 64);
 		break;
	case 0x14: /* Exchange event-handler */ 
		{	
			Bit16u oldSeg = mouse.sub_seg;
			Bit16u oldOfs = mouse.sub_ofs;
			Bit16u oldMask= mouse.sub_mask;
			// Set new values
			mouse.sub_mask= reg_cx;
			mouse.sub_seg = SegValue(es);
			mouse.sub_ofs = reg_dx;
			// Return old values
			reg_cx = oldMask;
			reg_dx = oldOfs;
			SegSet16(es,oldSeg);
		}
		break;		
	case 0x15: /* Get Driver storage space requirements */
		reg_bx = sizeof(mouse);
		break;
	case 0x16: /* Save driver state */
		{
			LOG(LOG_MOUSE,LOG_WARN)("Saving driver state...");
			PhysPt dest = SegPhys(es)+reg_dx;
			MEM_BlockWrite(dest, &mouse, sizeof(mouse));
		}
		break;
	case 0x17: /* load driver state */
		{
			LOG(LOG_MOUSE,LOG_WARN)("Loading driver state...");
			PhysPt src = SegPhys(es)+reg_dx;
			MEM_BlockRead(src, &mouse, sizeof(mouse));
		}
		break;
	case 0x1a:	/* Set mouse sensitivity */
		// ToDo : double mouse speed value
		Mouse_SetSensitivity(reg_bx,reg_cx,reg_dx);

		LOG(LOG_MOUSE,LOG_WARN)("Set sensitivity used with %d %d (%d)",reg_bx,reg_cx,reg_dx);
		break;
	case 0x1b:	/* Get mouse sensitivity */
		reg_bx = mouse.senv_x_val;
		reg_cx = mouse.senv_y_val;
		reg_dx = mouse.dspeed_val;

		LOG(LOG_MOUSE,LOG_WARN)("Get sensitivity %d %d",reg_bx,reg_cx);
		break;
	case 0x1c:	/* Set interrupt rate */
		/* Can't really set a rate this is host determined */
		break;
	case 0x1d:      /* Set display page number */
		mouse.page=reg_bl;
		break;
	case 0x1e:      /* Get display page number */
		reg_bx=mouse.page;
		break;
	case 0x1f:	/* Disable Mousedriver */
		/* ES:BX old mouse driver Zero at the moment TODO */ 
		reg_bx=0;
		SegSet16(es,0);	   
		mouse.enabled=false; /* Just for reporting not doing a thing with it */
		mouse.oldhidden=mouse.hidden;
		mouse.hidden=1;
		break;
	case 0x20:	/* Enable Mousedriver */
		mouse.enabled=true;
		mouse.hidden=mouse.oldhidden;
		break;
	case 0x22:      /* Set language for messages */
 			/*
			 *                        Values for mouse driver language:
			 * 
			 *                        00h     English
			 *                        01h     French
			 *                        02h     Dutch
			 *                        03h     German
			 *                        04h     Swedish
			 *                        05h     Finnish
			 *                        06h     Spanish
			 *                        07h     Portugese
			 *                        08h     Italian
			 *                
			 */
		mouse.language=reg_bx;
		break;
	case 0x23:      /* Get language for messages */
		reg_bx=mouse.language;
		break;
	case 0x24:	/* Get Software version and mouse type */
		reg_bx=0x805;	//Version 8.05 woohoo 
		reg_ch=0x04;	/* PS/2 type */
		reg_cl=0;		/* PS/2 (unused) */
		break;
	case 0x26: /* Get Maximum virtual coordinates */
		reg_bx=(mouse.enabled ? 0x0000 : 0xffff);
		reg_cx=(Bit16u)mouse.max_x;
		reg_dx=(Bit16u)mouse.max_y;
		break;
	case 0x2a:	/* Get cursor hot spot */
		reg_al=(Bit8u)-mouse.hidden;	// Microsoft uses a negative byte counter for cursor visibility
		reg_bx=(Bit16u)mouse.hotx;
		reg_cx=(Bit16u)mouse.hoty;
		reg_dx=0x04;	// PS/2 mouse type
		break;
	case 0x31: /* Get Current Minimum/Maximum virtual coordinates */
		reg_ax=(Bit16u)mouse.min_x;
		reg_bx=(Bit16u)mouse.min_y;
		reg_cx=(Bit16u)mouse.max_x;
		reg_dx=(Bit16u)mouse.max_y;
		break;
	default:
		LOG(LOG_MOUSE,LOG_ERROR)("Mouse Function %04X not implemented!",reg_ax);
		break;
	}
	return CBRET_NONE;
}

static Bitu MOUSE_BD_Handler(void) {
	// the stack contains offsets to register values
	Bit16u raxpt=real_readw(SegValue(ss),reg_sp+0x0a);
	Bit16u rbxpt=real_readw(SegValue(ss),reg_sp+0x08);
	Bit16u rcxpt=real_readw(SegValue(ss),reg_sp+0x06);
	Bit16u rdxpt=real_readw(SegValue(ss),reg_sp+0x04);

	// read out the actual values, registers ARE overwritten
	Bit16u rax=real_readw(SegValue(ds),raxpt);
	reg_ax=rax;
	reg_bx=real_readw(SegValue(ds),rbxpt);
	reg_cx=real_readw(SegValue(ds),rcxpt);
	reg_dx=real_readw(SegValue(ds),rdxpt);
//	LOG_MSG("MOUSE BD: %04X %X %X %X %d %d",reg_ax,reg_bx,reg_cx,reg_dx,POS_X,POS_Y);
	
	// some functions are treated in a special way (additional registers)
	switch (rax) {
		case 0x09:	/* Define GFX Cursor */
		case 0x16:	/* Save driver state */
		case 0x17:	/* load driver state */
			SegSet16(es,SegValue(ds));
			break;
		case 0x0c:	/* Define interrupt subroutine parameters */
		case 0x14:	/* Exchange event-handler */ 
			if (reg_bx!=0) SegSet16(es,reg_bx);
			else SegSet16(es,SegValue(ds));
			break;
		case 0x10:	/* Define screen region for updating */
			reg_cx=real_readw(SegValue(ds),rdxpt);
			reg_dx=real_readw(SegValue(ds),rdxpt+2);
			reg_si=real_readw(SegValue(ds),rdxpt+4);
			reg_di=real_readw(SegValue(ds),rdxpt+6);
			break;
		default:
			break;
	}

	INT33_Handler();

	// save back the registers, too
	real_writew(SegValue(ds),raxpt,reg_ax);
	real_writew(SegValue(ds),rbxpt,reg_bx);
	real_writew(SegValue(ds),rcxpt,reg_cx);
	real_writew(SegValue(ds),rdxpt,reg_dx);
	switch (rax) {
		case 0x1f:	/* Disable Mousedriver */
			real_writew(SegValue(ds),rbxpt,SegValue(es));
			break;
		case 0x14: /* Exchange event-handler */ 
			real_writew(SegValue(ds),rcxpt,SegValue(es));
			break;
		default:
			break;
	}

	reg_ax=rax;
	return CBRET_NONE;
}

static Bitu INT74_Handler(void) {
	if (mouse.events>0 && !mouse.in_UIR) {
		mouse.events--;
		/* Check for an active Interrupt Handler that will get called */
		if (mouse.sub_mask & mouse.event_queue[mouse.events].type) {
			reg_ax=mouse.event_queue[mouse.events].type;
			reg_bx=mouse.event_queue[mouse.events].buttons;
			reg_cx=POS_X;
			reg_dx=POS_Y;
			reg_si=static_cast<Bit16s>(mouse.mickey_x);
			reg_di=static_cast<Bit16s>(mouse.mickey_y);
			CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
			CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback))+7);
			CPU_Push16(RealSeg(uir_callback));
			CPU_Push16(RealOff(uir_callback));
			CPU_Push16(mouse.sub_seg);
			CPU_Push16(mouse.sub_ofs);
			mouse.in_UIR = true;
			//LOG(LOG_MOUSE,LOG_ERROR)("INT 74 %X",mouse.event_queue[mouse.events].type );
		} else if (useps2callback) {
			CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
			CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)));
			DoPS2Callback(mouse.event_queue[mouse.events].buttons, static_cast<Bit16s>(mouse.x), static_cast<Bit16s>(mouse.y));
		} else {
			SegSet16(cs, RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
			reg_ip = RealOff(CALLBACK_RealPointer(int74_ret_callback));
			//LOG(LOG_MOUSE,LOG_ERROR)("INT 74 not interested"); 
		}
	} else {
		SegSet16(cs, RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
		reg_ip = RealOff(CALLBACK_RealPointer(int74_ret_callback));
		//LOG(LOG_MOUSE,LOG_ERROR)("INT 74 no events");
	}
	return CBRET_NONE;
}

Bitu INT74_Ret_Handler(void) {
	if (mouse.events) {
		if (!mouse.timer_in_progress) {
			mouse.timer_in_progress = true;
			PIC_AddEvent(MOUSE_Limit_Events,MOUSE_DELAY);
		}
	}
	return CBRET_NONE;
}

Bitu UIR_Handler(void) {
	mouse.in_UIR = false;
	return CBRET_NONE;
}

void MOUSE_Init(Section* sec) {
	memset(&ps2_bios,0,sizeof(ps2_bios));
	Mouse_PS2ApplyDefaults(false);
	useps2callback = false;
	ps2callbackinit = false;
	ps2cbseg = ps2cbofs = 0;
	memset(&mouse_integration,0,sizeof(mouse_integration));
	mouse_integration.mode = MOUSE_INTEGRATION_NONE;
	mouse_integration.pointer_mode = MOUSE_POINTER_WINDOWS_ONLY;
	if (sec) {
		Section_prop *section = static_cast<Section_prop *>(sec);
		const char *mode = section->Get_string("mode");
		if (mode && !strcmp(mode,"win31"))
			mouse_integration.mode = MOUSE_INTEGRATION_WIN31;
		else if (mode && !strcmp(mode,"win95"))
			mouse_integration.mode = MOUSE_INTEGRATION_WIN95;
		const char *pointer = section->Get_string("pointer");
		if (pointer && !strcmp(pointer,"both"))
			mouse_integration.pointer_mode = MOUSE_POINTER_BOTH;
		else if (pointer && !strcmp(pointer,"morphos_only"))
			mouse_integration.pointer_mode = MOUSE_POINTER_MORPHOS_ONLY;
	}
	DOSBOX_ID_ResetInterface();
	if (Mouse_IntegrationConfigured()) {
		IO_RegisterReadHandler(DOSBOX_ID_BASE,DOSBOX_ID_Read,IO_MA,4);
		IO_RegisterWriteHandler(DOSBOX_ID_BASE,DOSBOX_ID_Write,IO_MA,4);
		LOG(LOG_MOUSE,LOG_NORMAL)("DOSBox-X mouse integration device enabled for %s",
			mouse_integration.mode == MOUSE_INTEGRATION_WIN31 ? "Windows 3.1" : "Windows 95");
	}

	// Callback for mouse interrupt 0x33
	call_int33=CALLBACK_Allocate();
//	RealPt i33loc=RealMake(CB_SEG+1,(call_int33*CB_SIZE)-0x10);
	RealPt i33loc=RealMake(DOS_GetMemory(0x1)-1,0x10);
	CALLBACK_Setup(call_int33,&INT33_Handler,CB_MOUSE,Real2Phys(i33loc),"Mouse");
	// Wasteland needs low(seg(int33))!=0 and low(ofs(int33))!=0
	real_writed(0,0x33<<2,i33loc);

	call_mouse_bd=CALLBACK_Allocate();
	CALLBACK_Setup(call_mouse_bd,&MOUSE_BD_Handler,CB_RETF8,
		PhysMake(RealSeg(i33loc),RealOff(i33loc)+2),"MouseBD");
	// pseudocode for CB_MOUSE (including the special backdoor entry point):
	//	jump near i33hd
	//	callback MOUSE_BD_Handler
	//	retf 8
	//  label i33hd:
	//	callback INT33_Handler
	//	iret


	// Callback for ps2 irq
	call_int74=CALLBACK_Allocate();
	CALLBACK_Setup(call_int74,&INT74_Handler,CB_IRQ12,"int 74");
	// pseudocode for CB_IRQ12:
	//	sti
	//	push ds
	//	push es
	//	pushad
	//	callback INT74_Handler
	//		ps2 or user callback if requested
	//		otherwise jumps to CB_IRQ12_RET
	//	push ax
	//	mov al, 0x20
	//	out 0xa0, al
	//	out 0x20, al
	//	pop	ax
	//	cld
	//	retf

	int74_ret_callback=CALLBACK_Allocate();
	CALLBACK_Setup(int74_ret_callback,&INT74_Ret_Handler,CB_IRQ12_RET,"int 74 ret");
	// pseudocode for CB_IRQ12_RET:
	//	cli
	//	mov al, 0x20
	//	out 0xa0, al
	//	out 0x20, al
	//	callback INT74_Ret_Handler
	//	popad
	//	pop es
	//	pop ds
	//	iret

	Bit8u hwvec=(MOUSE_IRQ>7)?(0x70+MOUSE_IRQ-8):(0x8+MOUSE_IRQ);
	RealSetVec(hwvec,CALLBACK_RealPointer(call_int74));

	// Callback for ps2 user callback handling
	useps2callback = false; ps2callbackinit = false;
 	call_ps2=CALLBACK_Allocate();
	CALLBACK_Setup(call_ps2,&PS2_Handler,CB_RETF,"ps2 bios callback");
	ps2_callback=CALLBACK_RealPointer(call_ps2);

	// Callback for mouse user routine return
	call_uir=CALLBACK_Allocate();
	CALLBACK_Setup(call_uir,&UIR_Handler,CB_RETF_CLI,"mouse uir ret");
	uir_callback=CALLBACK_RealPointer(call_uir);

	memset(&mouse,0,sizeof(mouse));
	mouse.hidden = 1; //Hide mouse on startup
	mouse.timer_in_progress = false;
	mouse.mode = 0xFF; //Non existing mode

   	mouse.sub_mask=0;
	mouse.sub_seg=0x6362;	// magic value
	mouse.sub_ofs=0;

	Mouse_ResetHardware();
	Mouse_Reset();
	Mouse_SetSensitivity(50,50,50);
}

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



#ifndef DOSBOX_MOUSE_H
#define DOSBOX_MOUSE_H


void Mouse_ShowCursor(void);
void Mouse_HideCursor(void);

bool Mouse_SetPS2State(bool use);
bool Mouse_PS2CallbackInstalled(void);
void Mouse_PS2Reset(void);
bool Mouse_PS2Initialize(Bit8u packet_size);
bool Mouse_PS2SetSamplingRate(Bit8u rate_code);
bool Mouse_PS2SetResolution(Bit8u resolution);
void Mouse_PS2SetScaling(bool scaling_21);
Bit8u Mouse_PS2GetType(void);
void Mouse_PS2GetStatus(Bit8u &status, Bit8u &resolution, Bit8u &sample_rate);

void Mouse_ChangePS2Callback(Bit16u pseg, Bit16u pofs);


void Mouse_CursorMoved(float xrel,float yrel,float x,float y,bool emulate);
void Mouse_CursorSet(float x,float y);
void Mouse_ButtonPressed(Bit8u button);
void Mouse_ButtonReleased(Bit8u button);

void Mouse_AutoLock(bool enable);
void Mouse_BeforeNewVideoMode(bool setmode);
void Mouse_AfterNewVideoMode(bool setmode);

/* DOSBox-X integration I/O device used by the Windows 3.1 and Windows 95
 * absolute-pointer drivers. */
bool Mouse_IntegrationConfigured(void);
bool Mouse_IntegrationGuestActive(void);
bool Mouse_IntegrationShowHostCursor(void);
bool Mouse_IntegrationShowGuestCursor(void);
void Mouse_IntegrationHostPosition(Bit16u x, Bit16u y, Bit16u width, Bit16u height);
void Mouse_IntegrationSetLocked(bool locked);
#if defined(__MORPHOS__)
void Mouse_IntegrationStateChanged(void);
#endif

#endif

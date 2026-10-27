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


#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dosbox.h"
#include "debug.h"
#include "cpu.h"
#include "video.h"
#include "pic.h"
#include "cpu.h"
#include "callback.h"
#include "inout.h"
#include "mixer.h"
#include "timer.h"
#include "dos_inc.h"
#include "setup.h"
#include "control.h"
#include "cross.h"
#include "programs.h"
#include "support.h"
#if defined(__WARPOS__)
#include "wos_prof.h"
#else
#define WOS_PROF_BEGIN(id) do {} while (0)
#define WOS_PROF_END(id) do {} while (0)
#endif
#include "mapper.h"
#include "ints/int10.h"
#include "render.h"
#include "pci_bus.h"
#ifdef __MORPHOS__
#include "ne2000.h"
#endif
#include "voodoo.h"
#ifdef __MORPHOS__
#include "morphos_platform.h"
#endif

Config * control;
MachineType machine;
SVGACards svgaCard;

/* The whole load of startups for all the subfunctions */
void MSG_Init(Section_prop *);
void LOG_StartUp(void);
void MEM_Init(Section *);
void PAGING_Init(Section *);
void IO_Init(Section * );
void CALLBACK_Init(Section*);
void PROGRAMS_Init(Section*);
//void CREDITS_Init(Section*);
void RENDER_Init(Section*);
void VGA_Init(Section*);

void DOS_Init(Section*);


void CPU_Init(Section*);

#if C_FPU
void FPU_Init(Section*);
#endif

void DMA_Init(Section*);

void MIXER_Init(Section*);
void MIDI_Init(Section*);
void HARDWARE_Init(Section*);

#if defined(PCI_FUNCTIONALITY_ENABLED)
void PCI_Init(Section*);
#endif


void KEYBOARD_Init(Section*);	//TODO This should setup INT 16 too but ok ;)
void JOYSTICK_Init(Section*);
void MOUSE_Init(Section*);
void SBLASTER_Init(Section*);
void GUS_Init(Section*);
void MPU401_Init(Section*);
void PCSPEAKER_Init(Section*);
void TANDYSOUND_Init(Section*);
void DISNEY_Init(Section*);
void SERIAL_Init(Section*);


#if C_IPX
void IPX_Init(Section*);
#endif

void SID_Init(Section* sec);

void PIC_Init(Section*);
void TIMER_Init(Section*);
void BIOS_Init(Section*);
void DEBUG_Init(Section*);
void CMOS_Init(Section*);

void MSCDEX_Init(Section*);
void DRIVES_Init(Section*);
void CDROM_Image_Init(Section*);

/* Dos Internal mostly */
void EMS_Init(Section*);
void XMS_Init(Section*);

void DOS_KeyboardLayout_Init(Section*);

void AUTOEXEC_Init(Section*);
void SHELL_Init(void);

void INT10_Init(Section*);

static LoopHandler * loop;

bool NetworkInited;

static Bit32u ticksRemain;
static Bit32u ticksLast;
static Bit32u ticksAdded;
Bit32s ticksDone;
Bit32u ticksScheduled;
bool ticksLocked;
void increaseticks();

#ifdef __MORPHOS__
/*
 * The original max-cycles controller measures host load in whole milliseconds
 * and subtracts a whole-millisecond sleep result.  On fast PowerPC hosts this
 * quantisation can make the same executable settle at very different cycle
 * counts.  Keep DOS/PIC scheduling at 1 ms, but measure only the auto-cycles
 * controller with the monotonic EClock microsecond source.
 */
static bool morphos_highres_max_cycles = true;
static bool morphos_cycle_stats = false;
static Bit64u morphos_auto_busy_us = 0;
static Bit64u morphos_auto_scheduled_us = 0;
static Bit64u morphos_auto_last_us = 0;
static Bit64u morphos_cycle_sum = 0;
static Bit32u morphos_cycle_updates = 0;
static Bit32u morphos_cycle_min = 0xffffffffu;
static Bit32u morphos_cycle_max = 0;

void DOSBOX_SetHighResolutionMaxCycles(bool enabled)
{
    morphos_highres_max_cycles = enabled;
}

void DOSBOX_SetCycleStats(bool enabled)
{
    morphos_cycle_stats = enabled;
}

void DOSBOX_ResetAutoCycleController(void)
{
    morphos_auto_busy_us = 0;
    morphos_auto_scheduled_us = 0;
    morphos_auto_last_us = MORPHOS_GetMicroseconds();
}

void DOSBOX_DumpCycleStats(void)
{
    if (!morphos_cycle_stats) return;
    if (!morphos_cycle_updates) {
        LOG_MSG("MorphOS max-cycles statistics: no controller updates");
        return;
    }
    LOG_MSG("MorphOS max-cycles statistics (%s clock): updates=%lu, min=%lu, max=%lu, average=%llu, final=%ld",
            morphos_highres_max_cycles ? "EClock microsecond" : "legacy millisecond",
            (unsigned long)morphos_cycle_updates,
            (unsigned long)morphos_cycle_min,
            (unsigned long)morphos_cycle_max,
            (unsigned long long)(morphos_cycle_sum / morphos_cycle_updates),
            (long)CPU_CycleMax);
}

static void MorphOS_RecordCycleValue(Bit32s value)
{
    if (!morphos_cycle_stats || value <= 0) return;
    const Bit32u current = (Bit32u)value;
    if (current < morphos_cycle_min) morphos_cycle_min = current;
    if (current > morphos_cycle_max) morphos_cycle_max = current;
    morphos_cycle_sum += current;
    morphos_cycle_updates++;
}
#endif

static Bitu Normal_Loop(void) {
	Bits ret;
	while (1) {
		WOS_PROF_BEGIN(WOS_PROF_PIC);
		const bool pic_run = PIC_RunQueue();
		WOS_PROF_END(WOS_PROF_PIC);
		if (pic_run) {
			WOS_PROF_BEGIN(WOS_PROF_CPU);
			ret = (*cpudecoder)();
			WOS_PROF_END(WOS_PROF_CPU);
			if (GCC_UNLIKELY(ret<0)) return 1;
			if (ret>0) {
				if (GCC_UNLIKELY(ret >= CB_MAX)) return 0;
				Bitu blah = (*CallBack_Handlers[ret])();
				if (GCC_UNLIKELY(blah)) return blah;
			}
#if C_DEBUG
			if (DEBUG_ExitLoop()) return 0;
#endif
		} else {
#if defined(__WARPOS__)
			/* Every SDL event pump is a PPC->68k call costing several ms on
			 * WarpOS; poll at 50 Hz instead of once per emulated millisecond. */
			static Bit32u wos_last_events = 0;
			const Bit32u wos_now = GetTicks();
			if (wos_now - wos_last_events >= 20) {
				wos_last_events = wos_now;
				WOS_PROF_BEGIN(WOS_PROF_EVENTS);
				GFX_Events();
				WOS_PROF_END(WOS_PROF_EVENTS);
			}
#else
			GFX_Events();
#endif
			if (ticksRemain>0) {
				TIMER_AddTick();
				ticksRemain--;
			} else {
				WOS_PROF_BEGIN(WOS_PROF_TICKS);
				increaseticks();
				WOS_PROF_END(WOS_PROF_TICKS);
#if defined(__WARPOS__)
				WOS_Prof_Poll();
#endif
				return 0;
			}
		}
	}
}

//For trying other delays
#ifdef __MORPHOS__
#define wrap_delay(a) MORPHOS_Delay(a)
#else
#define wrap_delay(a) SDL_Delay(a)
#endif

void increaseticks() { //Make it return ticksRemain and set it in the function above to remove the global variable.
#ifdef __MORPHOS__
	const bool morphos_use_highres_cycles = morphos_highres_max_cycles &&
	                                         CPU_CycleAutoAdjust &&
	                                         !CPU_SkipCycleAutoAdjust;
	/* Account only time spent running DOSBox work.  Time spent in the explicit
	 * idle sleep is removed below by advancing morphos_auto_last_us after sleep. */
	if (morphos_use_highres_cycles) {
		const Bit64u now_us = MORPHOS_GetMicroseconds();
		if (morphos_auto_last_us && now_us >= morphos_auto_last_us)
			morphos_auto_busy_us += now_us - morphos_auto_last_us;
		morphos_auto_last_us = now_us;
		morphos_auto_scheduled_us += Bit64u(ticksAdded) * 1000ULL;
	}
#endif

	if (GCC_UNLIKELY(ticksLocked)) { // For Fast Forward Mode
		ticksRemain=5;
		/* Reset any auto cycle guessing for this frame */
		ticksLast = GetTicks();
		ticksAdded = 0;
		ticksDone = 0;
		ticksScheduled = 0;
#ifdef __MORPHOS__
		DOSBOX_ResetAutoCycleController();
#endif
		return;
	}
	
	static Bit32s lastsleepDone = -1;
	static Bitu sleep1count = 0;

	Bit32u ticksNew;
	ticksNew = GetTicks();
	ticksScheduled += ticksAdded;
	if (ticksNew <= ticksLast) { //lower should not be possible, only equal.
		ticksAdded = 0;

#ifdef __MORPHOS__
		if (morphos_use_highres_cycles) {
			/* The old 2/3/4 ms sleep pattern exists solely to decorrelate a
			 * millisecond-resolution controller.  It adds run-to-run variance on
			 * G5, so the high-resolution controller always requests one tick. */
			wrap_delay(1);
		} else
#endif
		if (!CPU_CycleAutoAdjust || CPU_SkipCycleAutoAdjust || sleep1count < 3) {
			wrap_delay(1);
		} else {
			/* Certain configurations always give an exact sleepingtime of 1, this causes problems due to the fact that
			   dosbox keeps track of full blocks.
			   This code introduces some randomness to the time slept, which improves stability on those configurations
			 */
			static const Bit32u sleeppattern[] = { 2, 2, 3, 2, 2, 4, 2};
			static Bit32u sleepindex = 0;
			if (ticksDone != lastsleepDone) sleepindex = 0;
			wrap_delay(sleeppattern[sleepindex++]);
			sleepindex %= sizeof(sleeppattern) / sizeof(sleeppattern[0]);
		}
		Bit32s timeslept = GetTicks() - ticksNew;
#ifdef __MORPHOS__
		if (morphos_use_highres_cycles) {
			/* Exclude the explicit idle sleep from the busy-time accumulator. */
			morphos_auto_last_us = MORPHOS_GetMicroseconds();
		} else
#endif
		{
			// Count how many times in the current block (of 250 ms) the time slept was 1 ms
			if (CPU_CycleAutoAdjust && !CPU_SkipCycleAutoAdjust && timeslept == 1) sleep1count++;
		}
		lastsleepDone = ticksDone;

		// Update ticksDone with the time spent sleeping for the legacy controller.
		ticksDone -= timeslept;
		if (ticksDone < 0)
			ticksDone = 0;
		return; //0
	}

	//TicksNew > ticksLast
	ticksRemain = ticksNew-ticksLast;
	ticksLast = ticksNew;
	ticksDone += ticksRemain;
	if ( ticksRemain > 20 ) {
//		LOG(LOG_MISC,LOG_ERROR)("large remain %d",ticksRemain);
		ticksRemain = 20;
	}
	ticksAdded = ticksRemain;

	// Is the system in auto cycle mode guessing ? If not just exit. (It can be temporary disabled)
	if (!CPU_CycleAutoAdjust || CPU_SkipCycleAutoAdjust) return;

	Bit64u scheduled_us = Bit64u(ticksScheduled) * 1000ULL;
	Bit64u done_us = Bit64u(ticksDone > 0 ? ticksDone : 1) * 1000ULL;
#ifdef __MORPHOS__
	if (morphos_use_highres_cycles) {
		scheduled_us = morphos_auto_scheduled_us;
		done_us = morphos_auto_busy_us ? morphos_auto_busy_us : 1;
	}
#endif
	
	if (scheduled_us >= 250000ULL || done_us >= 250000ULL ||
	    (ticksAdded > 15 && scheduled_us >= 5000ULL)) {
		/* ratio we are aiming for is around 90% usage */
		const Bit64u target = Bit64u(CPU_CyclePercUsed) * 90ULL * 1024ULL / 100ULL / 100ULL;
		Bit32s ratio = (Bit32s)((scheduled_us * target) / done_us);
		Bit32s new_cmax = CPU_CycleMax;
		const Bit64s cproc = (Bit64s)CPU_CycleMax * (Bit64s)(scheduled_us / 1000ULL);
		double ratioremoved = 0.0; //increase scope for logging
		if (cproc > 0) {
			/* ignore the cycles added due to the IO delay code in order
			   to have smoother auto cycle adjustments */
			ratioremoved = (double) CPU_IODelayRemoved / (double) cproc;
			if (ratioremoved < 1.0) {
				double ratio_not_removed = 1 - ratioremoved;
				ratio = (Bit32s)((double)ratio * ratio_not_removed);

				/* Don't allow very high ratio which can cause us to lock as we don't scale down
				 * for very low ratios. High ratio might result because of timing resolution */
				if (scheduled_us >= 250000ULL && done_us < 10000ULL && ratio > 16384)
					ratio = 16384;

				// Limit the ratio even more when the cycles are already way above the realmode default.
				if (scheduled_us >= 250000ULL && done_us < 10000ULL && ratio > 5120 && CPU_CycleMax > 50000)
					ratio = 5120;

				// When downscaling multiple times in a row, ensure a minimum amount of downscaling
				if (ticksAdded > 15 && scheduled_us >= 5000ULL && scheduled_us <= 20000ULL && ratio > 800)
					ratio = 800;

				if (ratio <= 1024) {
					double r = (1.0 + ratio_not_removed) /
					           (ratio_not_removed + 1024.0 / static_cast<double>(ratio ? ratio : 1));
					new_cmax = 1 + static_cast<Bit32s>(CPU_CycleMax * r);
				} else {
					Bit64s ratio_with_removed = (Bit64s)((((double)ratio - 1024.0) * ratio_not_removed) + 1024.0);
					Bit64s cmax_scaled = (Bit64s)CPU_CycleMax * ratio_with_removed;
					new_cmax = (Bit32s)(1 + (CPU_CycleMax >> 1) + cmax_scaled / (Bit64s)2048);
				}
			}
		}

		if (new_cmax < CPU_CYCLES_LOWER_LIMIT)
			new_cmax = CPU_CYCLES_LOWER_LIMIT;

		/* ratios below 1% are considered to be dropouts due to
		   temporary load imbalance, the cycles adjusting is skipped */
		if (ratio > 10) {
			/* ratios below 12% along with a large time since the last update
			   has taken place are most likely caused by heavy load through a
			   different application, the cycles adjusting is skipped as well */
			if ((ratio > 120) || (done_us < 700000ULL)) {
				CPU_CycleMax = new_cmax;
				if (CPU_CycleLimit > 0) {
					if (CPU_CycleMax > CPU_CycleLimit) CPU_CycleMax = CPU_CycleLimit;
				} else if (CPU_CycleMax > 2000000) CPU_CycleMax = 2000000; //Hardcoded limit, if no limit was specified.
#ifdef __MORPHOS__
				MorphOS_RecordCycleValue(CPU_CycleMax);
#endif
			}
		}

		//Reset cycleguessing parameters.
		CPU_IODelayRemoved = 0;
		ticksDone = 0;
		ticksScheduled = 0;
#ifdef __MORPHOS__
		if (morphos_use_highres_cycles) DOSBOX_ResetAutoCycleController();
#endif
		lastsleepDone = -1;
		sleep1count = 0;
	} else if (ticksAdded > 15 && scheduled_us < 5000ULL) {
		/* ticksAdded > 15 but less than 5 ms was scheduled: lower the cycles,
		 * but retain the measurements for the next controller update. */
		CPU_CycleMax /= 3;
		if (CPU_CycleMax < CPU_CYCLES_LOWER_LIMIT)
			CPU_CycleMax = CPU_CYCLES_LOWER_LIMIT;
#ifdef __MORPHOS__
		MorphOS_RecordCycleValue(CPU_CycleMax);
#endif
	}
}

void DOSBOX_SetLoop(LoopHandler * handler) {
	loop=handler;
}

void DOSBOX_SetNormalLoop() {
	loop=Normal_Loop;
}

void DOSBOX_RunMachine(void){
	Bitu ret;
	do {
		ret=(*loop)();
	} while (!ret);
}

static void DOSBOX_UnlockSpeed( bool pressed ) {
	static bool autoadjust = false;
	if (pressed) {
		LOG_MSG("Fast Forward ON");
		ticksLocked = true;
		if (CPU_CycleAutoAdjust) {
			autoadjust = true;
			CPU_CycleAutoAdjust = false;
			CPU_CycleMax /= 3;
			if (CPU_CycleMax<1000) CPU_CycleMax=1000;
		}
	} else {
		LOG_MSG("Fast Forward OFF");
		ticksLocked = false;
		if (autoadjust) {
			autoadjust = false;
			CPU_CycleAutoAdjust = true;
		}
	}
}

static void DOSBOX_RealInit(Section * sec) {
	Section_prop * section=static_cast<Section_prop *>(sec);
	/* Initialize some dosbox internals */

	ticksRemain=0;
	ticksLast=GetTicks();
	ticksLocked = false;
#ifdef __MORPHOS__
	DOSBOX_ResetAutoCycleController();
#endif
	DOSBOX_SetLoop(&Normal_Loop);
	MSG_Init(section);

	MAPPER_AddHandler(DOSBOX_UnlockSpeed, MK_f12, MMOD2,"speedlock","Speedlock");
	std::string cmd_machine;
	if (control->cmdline->FindString("-machine",cmd_machine,true)){
		//update value in config (else no matching against suggested values
		section->HandleInputline(std::string("machine=") + cmd_machine);
	}

	std::string mtype(section->Get_string("machine"));
	svgaCard = SVGA_None;
	machine = MCH_VGA;
	int10.vesa_nolfb = false;
	int10.vesa_oldvbe = false;
	if      (mtype == "cga")      { machine = MCH_CGA; }
	else if (mtype == "tandy")    { machine = MCH_TANDY; }
	else if (mtype == "pcjr")     { machine = MCH_PCJR; }
	else if (mtype == "hercules") { machine = MCH_HERC; }
	else if (mtype == "ega")      { machine = MCH_EGA; }
//	else if (mtype == "vga")          { svgaCard = SVGA_S3Trio; }
	else if (mtype == "svga_s3")       { svgaCard = SVGA_S3Trio; }
	else if (mtype == "vesa_nolfb")   { svgaCard = SVGA_S3Trio; int10.vesa_nolfb = true;}
	else if (mtype == "vesa_oldvbe")   { svgaCard = SVGA_S3Trio; int10.vesa_oldvbe = true;}
	else if (mtype == "svga_et4000")   { svgaCard = SVGA_TsengET4K; }
	else if (mtype == "svga_et3000")   { svgaCard = SVGA_TsengET3K; }
//	else if (mtype == "vga_pvga1a")   { svgaCard = SVGA_ParadisePVGA1A; }
	else if (mtype == "svga_paradise") { svgaCard = SVGA_ParadisePVGA1A; }
	else if (mtype == "vgaonly")      { svgaCard = SVGA_None; }
	else E_Exit("DOSBOX:Unknown machine type %s",mtype.c_str());
}


void DOSBOX_Init(void) {
	Section_prop * secprop;
	Section_line * secline;
	Prop_int* Pint;
	Prop_hex* Phex;
	Prop_string* Pstring;
	Prop_bool* Pbool;
	Prop_multival* Pmulti;
	Prop_multival_remain* Pmulti_remain;

	NetworkInited = false;

	// Some frequently used option sets
	const char *rates[] = {  "44100", "48000", "32000","22050", "16000", "11025", "8000", "49716", 0 };
	const char *oplrates[] = {   "44100", "49716", "48000", "32000","22050", "16000", "11025", "8000", 0 };
	const char *ios[] = { "220", "240", "260", "280", "2a0", "2c0", "2e0", "300", 0 };
	const char *irqssb[] = { "7", "5", "3", "9", "10", "11", "12", 0 };
	const char *dmassb[] = { "1", "5", "0", "3", "6", "7", 0 };
	const char *iosgus[] = { "240", "220", "260", "280", "2a0", "2c0", "2e0", "300", 0 };
	const char *irqsgus[] = { "5", "3", "7", "9", "10", "11", "12", 0 };
	const char *dmasgus[] = { "3", "0", "1", "5", "6", "7", 0 };


	/* Setup all the different modules making up DOSBox */
	const char* machines[] = {
		"hercules", "cga", "tandy", "pcjr", "ega",
		"vgaonly", "svga_s3", "svga_et3000", "svga_et4000",
		"svga_paradise", "vesa_nolfb", "vesa_oldvbe", 0 };
	secprop=control->AddSection_prop("dosbox",&DOSBOX_RealInit);
	Pstring = secprop->Add_path("language",Property::Changeable::Always,"");
	Pstring->Set_help("Select another language file.");

	Pstring = secprop->Add_string("machine",Property::Changeable::OnlyAtStart,"svga_s3");
	Pstring->Set_values(machines);
	Pstring->Set_help("The type of machine DOSBox tries to emulate.");

	Pstring = secprop->Add_path("captures",Property::Changeable::Always,"capture");
	Pstring->Set_help("Directory where things like wave, midi, screenshot get captured.");

	Pbool = secprop->Add_bool("convertdrivefat",Property::Changeable::OnlyAtStart,true);
	Pbool->Set_help("Expose the first eligible mounted host directory as a virtual FAT16 hard disk when BOOT starts a guest OS.");
	Pint = secprop->Add_int("convert fat free space",Property::Changeable::OnlyAtStart,250);
	Pint->SetMinMax(0,2048);
	Pint->Set_help("Free space in megabytes reported by the converted host-directory FAT disk. A value of 0 creates the minimum FAT16 size.");
	Pint = secprop->Add_int("convert fat timeout",Property::Changeable::OnlyAtStart,4);
	Pint->SetMinMax(0,120);
	Pint->Set_help("Maximum seconds used to scan a mounted directory before guest boot. Zero disables the timeout.");

#if C_DEBUG
	LOG_StartUp();
#endif

	secprop->AddInitFunction(&IO_Init);//done
	secprop->AddInitFunction(&PAGING_Init);//done
	secprop->AddInitFunction(&MEM_Init);//done
	secprop->AddInitFunction(&HARDWARE_Init);//done
	Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle,16);
	Pint->SetMinMax(1,63);
	Pint->Set_help(
		"Amount of memory DOSBox has in megabytes.\n"
		"This value is best left at its default to avoid problems with some games,\n"
		"though few games might require a higher value.\n"
		"There is generally no speed advantage when raising this value.");
	secprop->AddInitFunction(&CALLBACK_Init);
	secprop->AddInitFunction(&PIC_Init);//done
	secprop->AddInitFunction(&PROGRAMS_Init);
	secprop->AddInitFunction(&TIMER_Init);//done
	secprop->AddInitFunction(&CMOS_Init);//done

	secprop=control->AddSection_prop("render",&RENDER_Init,true);
	Pint = secprop->Add_int("frameskip",Property::Changeable::Always,0);
	Pint->SetMinMax(0,10);
	Pint->Set_help("How many frames DOSBox skips before drawing one.");

	Pbool = secprop->Add_bool("aspect",Property::Changeable::Always,false);
	Pbool->Set_help("Do aspect correction, if your output method doesn't support scaling this can slow things down!");

	Pmulti = secprop->Add_multi("scaler",Property::Changeable::Always," ");
	Pmulti->SetValue("normal2x");
	Pmulti->Set_help("Scaler used to enlarge/enhance low resolution modes. If 'forced' is appended,\n"
	                 "then the scaler will be used even if the result might not be desired.\n"
					 "To fit a scaler in the resolution used at full screen may require a border or side bars,\n"
					 "to fill the screen entirely, depending on your hardware, a different scaler/fullresolution might work.");
	Pstring = Pmulti->GetSection()->Add_string("type",Property::Changeable::Always,"normal2x");

	const char *scalers[] = {
		"none", "normal2x", "normal3x",
#if RENDER_USE_ADVANCED_SCALERS>2
		"advmame2x", "advmame3x", "advinterp2x", "advinterp3x", "hq2x", "hq3x", "2xsai", "super2xsai", "supereagle",
#endif
#if RENDER_USE_ADVANCED_SCALERS>0
		"tv2x", "tv3x", "rgb2x", "rgb3x", "scan2x", "scan3x",
#endif
		0 };
	Pstring->Set_values(scalers);

	const char* force[] = { "", "forced", 0 };
	Pstring = Pmulti->GetSection()->Add_string("force",Property::Changeable::Always,"");
	Pstring->Set_values(force);
#if C_OPENGL
	Pstring = secprop->Add_path("glshader",Property::Changeable::Always,"none");
	Pstring->Set_help("Path to GLSL shader source to use with OpenGL output (\"none\" to disable).\n"
					  "Can be either an absolute path, a file in the \"glshaders\" subdirectory\n"
					  "of the DOSBox configuration directory, or one of the built-in shaders:\n"
					  "advinterp2x, advinterp3x, advmame2x, advmame3x, rgb2x, rgb3x, scan2x,\n"
					  "scan3x, tv2x, tv3x, sharp.");
#endif

	secprop=control->AddSection_prop("cpu",&CPU_Init,true);//done
	const char* cores[] = { "auto",
#if (C_DYNAMIC_X86) || (C_DYNREC)
		"dynamic",
#endif
		"normal", "simple",0 };
	Pstring = secprop->Add_string("core",Property::Changeable::WhenIdle,"auto");
	Pstring->Set_values(cores);
	Pstring->Set_help("CPU Core used in emulation. auto will switch to dynamic if available and\n"
		"appropriate.");

	const char* cputype_values[] = { "auto", "386", "386_slow", "486_slow", "pentium_slow", "386_prefetch", 0};
	Pstring = secprop->Add_string("cputype",Property::Changeable::Always,"auto");
	Pstring->Set_values(cputype_values);
	Pstring->Set_help("CPU Type used in emulation. auto is the fastest choice.");


	Pmulti_remain = secprop->Add_multiremain("cycles",Property::Changeable::Always," ");
	Pmulti_remain->Set_help(
		"Amount of instructions DOSBox tries to emulate each millisecond.\n"
		"Setting this value too high results in sound dropouts and lags.\n"
		"Cycles can be set in 3 ways:\n"
		"  'auto'          tries to guess what a game needs.\n"
		"                  It usually works, but can fail for certain games.\n"
		"  'fixed #number' will set a fixed amount of cycles. This is what you usually\n"
		"                  need if 'auto' fails (Example: fixed 4000).\n"
		"  'max'           will allocate as much cycles as your computer is able to\n"
		"                  handle.");

	const char* cyclest[] = { "auto","fixed","max","%u",0 };
	Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::Always,"auto");
	Pmulti_remain->SetValue("auto");
	Pstring->Set_values(cyclest);

	Pstring = Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::Always,"");

	Pint = secprop->Add_int("cycleup",Property::Changeable::Always,10);
	Pint->SetMinMax(1,1000000);
	Pint->Set_help("Amount of cycles to decrease/increase with keycombos.(CTRL-F11/CTRL-F12)");

	Pint = secprop->Add_int("cycledown",Property::Changeable::Always,20);
	Pint->SetMinMax(1,1000000);
	Pint->Set_help("Setting it lower than 100 will be a percentage.");

#if C_FPU
	secprop->AddInitFunction(&FPU_Init);
#endif
	secprop->AddInitFunction(&DMA_Init);//done
	secprop->AddInitFunction(&VGA_Init);
	secprop->AddInitFunction(&KEYBOARD_Init);


#if defined(PCI_FUNCTIONALITY_ENABLED)
	secprop=control->AddSection_prop("pci",&PCI_Init,false); //PCI bus

#ifdef __MORPHOS__
	secprop=control->AddSection_prop("ethernet",&NE2000_Init,false);
	Pbool=secprop->Add_bool("ne2000",Property::Changeable::OnlyAtStart,false);
	Pbool->Set_help("Enable an emulated Novell NE2000 ISA Ethernet adapter using a native MorphOS SANA-II backend.");
	Phex=secprop->Add_hex("nicbase",Property::Changeable::OnlyAtStart,0x300);
	Phex->Set_help("NE2000 I/O base address. 300 is the common default.");
	Pint=secprop->Add_int("nicirq",Property::Changeable::OnlyAtStart,3);
	Pint->Set_help("NE2000 interrupt request line.");
	Pstring=secprop->Add_string("macaddr",Property::Changeable::OnlyAtStart,"AC:DE:48:88:99:AA");
	Pstring->Set_help("Compatibility setting. The native MorphOS shared SANA-II backend always uses the station address of the selected unit so the host network stack can remain online.");
	Pstring=secprop->Add_string("sana2_device",Property::Changeable::OnlyAtStart,"DEVS:Networks/rtl8139.device");
	Pstring->Set_help("MorphOS SANA-II network device path.");
	Pint=secprop->Add_int("sana2_unit",Property::Changeable::OnlyAtStart,0);
	Pint->Set_help("SANA-II device unit.");
#endif

	secprop=control->AddSection_prop("voodoo",&VOODOO_Init,false);
#if defined(__WARPOS__)
	const char *voodoo_cards[] = { "false", "software", "minigl", "auto", 0 };
	Pstring=secprop->Add_string("voodoo_card",Property::Changeable::OnlyAtStart,"auto");
	Pstring->Set_values(voodoo_cards);
	Pstring->Set_help("Enable low-level 3dfx Voodoo Graphics SST-1 emulation. software uses the complete DOSBox-X/Aaron Giles rasterizer and shows the result through the normal DOSBox screen. minigl translates triangle, texture, depth, blend, clear and swap operations to MiniGL (minigl_ppc.dll) on its own screen. auto selects minigl.");
	const char *voodoo_memory[] = { "4", "12", 0 };
	Pint=secprop->Add_int("voodoo_memsize",Property::Changeable::OnlyAtStart,4);
	Pint->Set_values(voodoo_memory);
	Pint->Set_help("Voodoo memory configuration in MB: 4 provides one 2 MB TMU; 12 provides two 4 MB TMUs and 4 MB framebuffer memory.");
	Pbool=secprop->Add_bool("voodoo_minigl_fullscreen",Property::Changeable::OnlyAtStart,true);
	Pbool->Set_help("Open the MiniGL Voodoo display as a fullscreen screen (true) or as a window on the Workbench (false).");
	const char *voodoo_depths[] = { "16", "32", 0 };
	Pint=secprop->Add_int("voodoo_minigl_depth",Property::Changeable::OnlyAtStart,16);
	Pint->Set_values(voodoo_depths);
	Pint->Set_help("Pixel depth of the MiniGL Voodoo screen. The emulated Voodoo framebuffer is 16-bit.");
#else
	const char *voodoo_cards[] = { "false", "software", "tinygl", 0 };
	Pstring=secprop->Add_string("voodoo_card",Property::Changeable::OnlyAtStart,"false");
	Pstring->Set_values(voodoo_cards);
	Pstring->Set_help("Enable low-level 3dfx Voodoo Graphics SST-1 emulation. software uses the complete DOSBox-X/Aaron Giles rasterizer. tinygl uses the same complete SST-1 core and translates triangle, texture, depth, blend, clear, swap and LFB raster operations directly to native MorphOS TinyGL.");
	const char *voodoo_submit_modes[] = { "immediate", "arrays", "vbo", "vbo-indexed", 0 };
	Pstring=secprop->Add_string("voodoo_tinygl_submission",Property::Changeable::OnlyAtStart,"arrays");
	Pstring->Set_values(voodoo_submit_modes);
	Pstring->Set_help("TinyGL triangle submission backend. immediate uses GLBegin/GLEnd for compatibility. arrays batches triangles in aligned CPU memory and submits them with glDrawArrays. vbo streams the same batches through a TinyGL vertex buffer object using the R300-safe pointer-offset path. vbo-indexed uses glDrawElements so the VBO attribute pointers are configured once per upload instead of once per draw. The setting is visible in the MorphOS configuration GUI and applies after restarting DOSBox.");
	const char *voodoo_tuning_modes[] = { "safe", "fast", "max", 0 };
	Pstring=secprop->Add_string("voodoo_tinygl_tuning",Property::Changeable::OnlyAtStart,"fast");
	Pstring->Set_values(voodoo_tuning_modes);
	Pstring->Set_help("TinyGL performance profile. safe preserves the legacy reciprocal texture-coordinate path and v5.8 limits. fast uses exact SST-scaled homogeneous S/T/Q coordinates, cached texture setup, reduced vertex colour work, larger batches/cache and conservative off-screen rejection. max uses the same geometry optimisations with the largest batch/cache and more graphics memory.");
	Pbool=secprop->Add_bool("voodoo_tinygl_statecache",Property::Changeable::OnlyAtStart,true);
	Pbool->Set_help("Cache the last decoded SST draw state while building the VBO queue. Disable this switch for direct A/B performance and compatibility testing. Applies after restarting DOSBox.");
	Pbool=secprop->Add_bool("voodoo_tinygl_shader_support",Property::Changeable::OnlyAtStart,false);
	Pbool->Set_help("Use the optional R300 TinyGL GLSL renderer for SST texture combiners, dual TMU, alpha/chroma tests and fog. The fixed-function renderer remains the automatic fallback if shader compilation or runtime capability checks fail. Requires vbo or vbo-indexed and applies after restarting DOSBox.");
	Pbool=secprop->Add_bool("voodoo_debug",Property::Changeable::OnlyAtStart,false);
	Pbool->Set_help("Enable Voodoo diagnostic logging and expensive runtime data collection. Keep disabled for normal use to avoid unnecessary CPU overhead. Applies after restarting DOSBox.");
	const char *voodoo_memory[] = { "4", "12", 0 };
	Pint=secprop->Add_int("voodoo_memsize",Property::Changeable::OnlyAtStart,4);
	Pint->Set_values(voodoo_memory);
	Pint->Set_help("Voodoo memory configuration in MB: 4 provides one 2 MB TMU; 12 provides two 4 MB TMUs and 4 MB framebuffer memory.");
	Pbool=secprop->Add_bool("voodoo_bilinear_filtering",Property::Changeable::OnlyAtStart,true);
	Pbool->Set_help("Compatibility option retained for existing configurations. Texture filtering is controlled by the emulated SST-1 registers.");
#endif // __WARPOS__
#endif


	secprop=control->AddSection_prop("mixer",&MIXER_Init);
	Pbool = secprop->Add_bool("nosound",Property::Changeable::OnlyAtStart,false);
	Pbool->Set_help("Enable silent mode, sound is still emulated though.");

	Pint = secprop->Add_int("rate",Property::Changeable::OnlyAtStart,44100);
	Pint->Set_values(rates);
	Pint->Set_help("Mixer sample rate, setting any device's rate higher than this will probably lower their sound quality.");

	const char *blocksizes[] = {
		 "1024", "2048", "4096", "8192", "512", "256", 0};
	Pint = secprop->Add_int("blocksize",Property::Changeable::OnlyAtStart,1024);
	Pint->Set_values(blocksizes);
	Pint->Set_help("Mixer block size, larger blocks might help sound stuttering but sound will also be more lagged.");

	Pint = secprop->Add_int("prebuffer",Property::Changeable::OnlyAtStart,25);
	Pint->SetMinMax(0,100);
	Pint->Set_help("How many milliseconds of data to keep on top of the blocksize.");

	secprop=control->AddSection_prop("midi",&MIDI_Init,true);//done
	secprop->AddInitFunction(&MPU401_Init,true);//done

	const char* mputypes[] = { "intelligent", "uart", "none",0};
	// FIXME: add some way to offer the actually available choices.
	const char *devices[] = { "default", "win32", "alsa", "oss", "coreaudio", "coremidi","none", 0};
	Pstring = secprop->Add_string("mpu401",Property::Changeable::WhenIdle,"intelligent");
	Pstring->Set_values(mputypes);
	Pstring->Set_help("Type of MPU-401 to emulate.");

	Pstring = secprop->Add_string("mididevice",Property::Changeable::WhenIdle,"default");
	Pstring->Set_values(devices);
	Pstring->Set_help("Device that will receive the MIDI data from MPU-401.");

	Pstring = secprop->Add_string("midiconfig",Property::Changeable::WhenIdle,"");
	Pstring->Set_help("Special configuration options for the device driver. This is usually the id or part of the name of the device you want to use\n"
	                  "(find the id/name with mixer/listmidi).\n"
	                  "Or in the case of coreaudio, you can specify a soundfont here.\n"
	                  "When using a Roland MT-32 rev. 0 as midi output device, some games may require a delay in order to prevent 'buffer overflow' issues.\n"
	                  "In that case, add 'delaysysex', for example: midiconfig=2 delaysysex\n"
	                  "See the README/Manual for more details.");

#if C_DEBUG
	secprop=control->AddSection_prop("debug",&DEBUG_Init);
#endif

	secprop=control->AddSection_prop("sblaster",&SBLASTER_Init,true);//done

	const char* sbtypes[] = { "sb1", "sb2", "sbpro1", "sbpro2", "sb16", "gb", "none", 0 };
	Pstring = secprop->Add_string("sbtype",Property::Changeable::WhenIdle,"sb16");
	Pstring->Set_values(sbtypes);
	Pstring->Set_help("Type of Soundblaster to emulate. gb is Gameblaster.");

	Phex = secprop->Add_hex("sbbase",Property::Changeable::WhenIdle,0x220);
	Phex->Set_values(ios);
	Phex->Set_help("The IO address of the soundblaster.");

	Pint = secprop->Add_int("irq",Property::Changeable::WhenIdle,7);
	Pint->Set_values(irqssb);
	Pint->Set_help("The IRQ number of the soundblaster.");

	Pint = secprop->Add_int("dma",Property::Changeable::WhenIdle,1);
	Pint->Set_values(dmassb);
	Pint->Set_help("The DMA number of the soundblaster.");

	Pint = secprop->Add_int("hdma",Property::Changeable::WhenIdle,5);
	Pint->Set_values(dmassb);
	Pint->Set_help("The High DMA number of the soundblaster.");

	Pbool = secprop->Add_bool("sbmixer",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("Allow the soundblaster mixer to modify the DOSBox mixer.");

	const char* oplmodes[]={ "auto", "cms", "opl2", "dualopl2", "opl3", "opl3gold", "none", 0};
	Pstring = secprop->Add_string("oplmode",Property::Changeable::WhenIdle,"auto");
	Pstring->Set_values(oplmodes);
	Pstring->Set_help("Type of OPL emulation. On 'auto' the mode is determined by sblaster type. All OPL modes are Adlib-compatible, except for 'cms'.");

	const char* oplemus[]={ "default", "compat", "fast", "mame", 0};
	Pstring = secprop->Add_string("oplemu",Property::Changeable::WhenIdle,"default");
	Pstring->Set_values(oplemus);
	Pstring->Set_help("Provider for the OPL emulation. compat might provide better quality (see oplrate as well).");

	Pint = secprop->Add_int("oplrate",Property::Changeable::WhenIdle,44100);
	Pint->Set_values(oplrates);
	Pint->Set_help("Sample rate of OPL music emulation. Use 49716 for highest quality (set the mixer rate accordingly).");


	secprop=control->AddSection_prop("gus",&GUS_Init,true); //done
	Pbool = secprop->Add_bool("gus",Property::Changeable::WhenIdle,false);
	Pbool->Set_help("Enable the Gravis Ultrasound emulation.");

	Pint = secprop->Add_int("gusrate",Property::Changeable::WhenIdle,44100);
	Pint->Set_values(rates);
	Pint->Set_help("Sample rate of Ultrasound emulation.");

	Phex = secprop->Add_hex("gusbase",Property::Changeable::WhenIdle,0x240);
	Phex->Set_values(iosgus);
	Phex->Set_help("The IO base address of the Gravis Ultrasound.");

	Pint = secprop->Add_int("gusirq",Property::Changeable::WhenIdle,5);
	Pint->Set_values(irqsgus);
	Pint->Set_help("The IRQ number of the Gravis Ultrasound.");

	Pint = secprop->Add_int("gusdma",Property::Changeable::WhenIdle,3);
	Pint->Set_values(dmasgus);
	Pint->Set_help("The DMA channel of the Gravis Ultrasound.");

	Pstring = secprop->Add_string("ultradir",Property::Changeable::WhenIdle,"C:\\ULTRASND");
	Pstring->Set_help(
		"Path to Ultrasound directory. In this directory\n"
		"there should be a MIDI directory that contains\n"
		"the patch files for GUS playback. Patch sets used\n"
		"with Timidity should work fine.");

	secprop = control->AddSection_prop("speaker",&PCSPEAKER_Init,true);//done
	Pbool = secprop->Add_bool("pcspeaker",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("Enable PC-Speaker emulation.");

	Pint = secprop->Add_int("pcrate",Property::Changeable::WhenIdle,44100);
	Pint->Set_values(rates);
	Pint->Set_help("Sample rate of the PC-Speaker sound generation.");

	secprop->AddInitFunction(&TANDYSOUND_Init,true);//done
	const char* tandys[] = { "auto", "on", "off", 0};
	Pstring = secprop->Add_string("tandy",Property::Changeable::WhenIdle,"auto");
	Pstring->Set_values(tandys);
	Pstring->Set_help("Enable Tandy Sound System emulation. For 'auto', emulation is present only if machine is set to 'tandy'.");

	Pint = secprop->Add_int("tandyrate",Property::Changeable::WhenIdle,44100);
	Pint->Set_values(rates);
	Pint->Set_help("Sample rate of the Tandy 3-Voice generation.");

	secprop->AddInitFunction(&DISNEY_Init,true);//done

	Pbool = secprop->Add_bool("disney",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("Enable Disney Sound Source emulation. (Covox Voice Master and Speech Thing compatible).");

	secprop=control->AddSection_prop("joystick",&BIOS_Init,false);//done
	secprop->AddInitFunction(&INT10_Init);
	secprop->AddInitFunction(&JOYSTICK_Init,true);
	const char* joytypes[] = { "auto", "2axis", "4axis", "4axis_2", "fcs", "ch", "none",0};
	Pstring = secprop->Add_string("joysticktype",Property::Changeable::WhenIdle,"auto");
	Pstring->Set_values(joytypes);
	Pstring->Set_help(
		"Type of joystick to emulate: auto (default), none,\n"
		"2axis (supports two joysticks),\n"
		"4axis (supports one joystick, first joystick used),\n"
		"4axis_2 (supports one joystick, second joystick used),\n"
		"fcs (Thrustmaster), ch (CH Flightstick).\n"
		"none disables joystick emulation.\n"
		"auto chooses emulation depending on real joystick(s).\n"
		"(Remember to reset dosbox's mapperfile if you saved it earlier)");

	Pbool = secprop->Add_bool("timed",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("enable timed intervals for axis. Experiment with this option, if your joystick drifts (away).");

	Pbool = secprop->Add_bool("autofire",Property::Changeable::WhenIdle,false);
	Pbool->Set_help("continuously fires as long as you keep the button pressed.");

	Pbool = secprop->Add_bool("swap34",Property::Changeable::WhenIdle,false);
	Pbool->Set_help("swap the 3rd and the 4th axis. Can be useful for certain joysticks.");

	Pbool = secprop->Add_bool("buttonwrap",Property::Changeable::WhenIdle,false);
	Pbool->Set_help("enable button wrapping at the number of emulated buttons.");
	
	Pbool = secprop->Add_bool("circularinput",Property::Changeable::WhenIdle,false);
	Pbool->Set_help("enable translation of circular input to square output.\n"
	                "Try enabling this if your left analog stick can only move in a circle.");

	Pint = secprop->Add_int("deadzone",Property::Changeable::WhenIdle,10);
	Pint->SetMinMax(0,100);
	Pint->Set_help("the percentage of motion to ignore. 100 turns the stick into a digital one.");

	/* Keep mouse initialization after INT10.  DOSBox-X provides separate
	 * guest drivers for Windows 3.1 and Windows 95, both using its integration
	 * I/O device at ports 28h-2Bh. */
	const char *mouse_integration_modes[] = { "none", "win31", "win95", 0 };
	secprop=control->AddSection_prop("mouse_integration",&MOUSE_Init,true);
	Pstring = secprop->Add_string("mode",Property::Changeable::OnlyAtStart,"none");
	Pstring->Set_values(mouse_integration_modes);
	Pstring->Set_help("Absolute mouse integration mode. none uses the normal captured mouse. win31 and win95 enable the DOSBox-X integration device at I/O ports 28h-2Bh for the matching guest driver.");
	const char *mouse_pointer_modes[] = { "windows_only", "both", "morphos_only", 0 };
	Pstring = secprop->Add_string("pointer",Property::Changeable::OnlyAtStart,"windows_only");
	Pstring->Set_values(mouse_pointer_modes);
	Pstring->Set_help("Pointer display while an integration driver is active. Windows only hides the MorphOS pointer. Both shows the Windows and MorphOS pointers. MorphOS only shows the MorphOS pointer and suppresses the emulated S3 hardware cursor used by Windows.");

	secprop=control->AddSection_prop("serial",&SERIAL_Init,true);
	const char* serials[] = { "dummy", "disabled", "modem", "nullmodem",
	                          "directserial",0 };

	Pmulti_remain = secprop->Add_multiremain("serial1",Property::Changeable::WhenIdle," ");
	Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"dummy");
	Pmulti_remain->SetValue("dummy");
	Pstring->Set_values(serials);
	Pstring = Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
	Pmulti_remain->Set_help(
		"set type of device connected to com port.\n"
		"Can be disabled, dummy, modem, nullmodem, directserial.\n"
		"Additional parameters must be in the same line in the form of\n"
		"parameter:value. Parameter for all types is irq (optional).\n"
		"for directserial: realport (required), rxdelay (optional).\n"
		"                 (realport:COM1 realport:ttyS0).\n"
		"for modem: listenport (optional).\n"
		"for nullmodem: server, rxdelay, txdelay, telnet, usedtr,\n"
		"               transparent, port, inhsocket (all optional).\n"
		"Example: serial1=modem listenport:5000");
#ifdef __MORPHOS__
	Pstring = secprop->Add_string("serial1_device",Property::Changeable::WhenIdle,"serial.device");
	Pstring->Set_help("MorphOS device used by directserial.");
	Pint = secprop->Add_int("serial1_unit",Property::Changeable::WhenIdle,0);
	Pint->SetMinMax(0,255);
	Pint->Set_help("MorphOS device unit used by directserial.");
#endif

	Pmulti_remain = secprop->Add_multiremain("serial2",Property::Changeable::WhenIdle," ");
	Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"dummy");
	Pmulti_remain->SetValue("dummy");
	Pstring->Set_values(serials);
	Pstring = Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
	Pmulti_remain->Set_help("see serial1");
#ifdef __MORPHOS__
	Pstring = secprop->Add_string("serial2_device",Property::Changeable::WhenIdle,"serial.device");
	Pstring->Set_help("MorphOS device used by directserial.");
	Pint = secprop->Add_int("serial2_unit",Property::Changeable::WhenIdle,1);
	Pint->SetMinMax(0,255);
	Pint->Set_help("MorphOS device unit used by directserial.");
#endif

	Pmulti_remain = secprop->Add_multiremain("serial3",Property::Changeable::WhenIdle," ");
	Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"disabled");
	Pmulti_remain->SetValue("disabled");
	Pstring->Set_values(serials);
	Pstring = Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
	Pmulti_remain->Set_help("see serial1");
#ifdef __MORPHOS__
	Pstring = secprop->Add_string("serial3_device",Property::Changeable::WhenIdle,"serial.device");
	Pstring->Set_help("MorphOS device used by directserial.");
	Pint = secprop->Add_int("serial3_unit",Property::Changeable::WhenIdle,2);
	Pint->SetMinMax(0,255);
	Pint->Set_help("MorphOS device unit used by directserial.");
#endif

	Pmulti_remain = secprop->Add_multiremain("serial4",Property::Changeable::WhenIdle," ");
	Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"disabled");
	Pmulti_remain->SetValue("disabled");
	Pstring->Set_values(serials);
	Pstring = Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
	Pmulti_remain->Set_help("see serial1");
#ifdef __MORPHOS__
	Pstring = secprop->Add_string("serial4_device",Property::Changeable::WhenIdle,"serial.device");
	Pstring->Set_help("MorphOS device used by directserial.");
	Pint = secprop->Add_int("serial4_unit",Property::Changeable::WhenIdle,3);
	Pint->SetMinMax(0,255);
	Pint->Set_help("MorphOS device unit used by directserial.");
#endif


	/* All the DOS Related stuff, which will eventually start up in the shell */
	secprop=control->AddSection_prop("dos",&DOS_Init,false);//done
	secprop->AddInitFunction(&XMS_Init,true);//done
	Pbool = secprop->Add_bool("xms",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("Enable XMS support.");

	secprop->AddInitFunction(&EMS_Init,true);//done
	const char* ems_settings[] = { "true", "emsboard", "emm386", "false", 0};
	Pstring = secprop->Add_string("ems",Property::Changeable::WhenIdle,"true");
	Pstring->Set_values(ems_settings);
	Pstring->Set_help("Enable EMS support. The default (=true) provides the best\n"
		"compatibility but certain applications may run better with\n"
		"other choices, or require EMS support to be disabled (=false)\n"
		"to work at all.");

	Pbool = secprop->Add_bool("umb",Property::Changeable::WhenIdle,true);
	Pbool->Set_help("Enable UMB support.");

	secprop->AddInitFunction(&DOS_KeyboardLayout_Init,true);
	Pstring = secprop->Add_string("keyboardlayout",Property::Changeable::WhenIdle, "auto");
	Pstring->Set_help("Language code of the keyboard layout (or none).");

	// Mscdex
	secprop->AddInitFunction(&MSCDEX_Init);
	secprop->AddInitFunction(&DRIVES_Init);
	secprop->AddInitFunction(&CDROM_Image_Init);
#if C_IPX
	secprop=control->AddSection_prop("ipx",&IPX_Init,true);
	Pbool = secprop->Add_bool("ipx",Property::Changeable::WhenIdle, false);
	Pbool->Set_help("Enable ipx over UDP/IP emulation.");
#endif
//	secprop->AddInitFunction(&CREDITS_Init);

	//TODO ?
	secline=control->AddSection_line("autoexec",&AUTOEXEC_Init);
	MSG_Add("AUTOEXEC_CONFIGFILE_HELP",
		"Lines in this section will be run at startup.\n"
		"You can put your MOUNT lines here.\n"
	);
	MSG_Add("CONFIGFILE_INTRO",
	        "# This is the configuration file for DOSBox %s. (Please use the latest version of DOSBox)\n"
	        "# Lines starting with a # are comment lines and are ignored by DOSBox.\n"
	        "# They are used to (briefly) document the effect of each option.\n");
	MSG_Add("CONFIG_SUGGESTED_VALUES", "Possible values");

	control->SetStartUp(&SHELL_Init);
}

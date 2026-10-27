# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A MorphOS / WarpOS (PowerPC, Amiga-family) port of DOSBox 0.74 (`VERSION` = 0.74-2), built on the upstream DOSBox SVN tree (autotools, C++). Upstream layout is unchanged; the port is a layer of `#ifdef __MORPHOS__` changes on top of it. Upstream docs: `README`, `INSTALL`, `docs/PORTING` (memory/speed tuning knobs for constrained platforms).

## Build

Standard autotools flow (`autogen.sh` regenerates `configure`; a generated `configure` and `config.h` are checked in):

```
./autogen.sh        # only if configure.ac / Makefile.am changed
./configure
make
```

Useful `configure` switches: `--disable-dynamic-core`, `--disable-dynrec`, `--disable-dynamic-x86`, `--disable-fpu-x86`, `--disable-core-inline`. On PowerPC the dynamic core is `core_dynrec` with `src/cpu/core_dynrec/risc_ppc.h`.

There is no test suite or linter in the repo.

## MorphOS-specific architecture

- `__MORPHOS__` guards mark port changes in `src/dosbox.cpp` (main loop, timing/sleep via `MORPHOS_GetMicroseconds` / `MORPHOS_Delay`), `src/gui/render.cpp`, `src/gui/midi.cpp`, `src/hardware/ipx*.cpp` (SDLNet calls remapped to `MORPHOSNet_*`), and headers `include/{timer,cross,callback,mixer}.h`. `include/timer.h` redefines `GetTicks()` to `MORPHOS_GetTicks()`.
- The port has two frontends, selected by the automake conditional `MORPHOS_NATIVE`:
  - **SDL frontend** (default): `src/gui/{sdlmain,sdl_mapper,sdl_gui}.cpp` plus `libs/gui_tk`.
  - **Native frontend**: `src/dosbox.cpp` + `platform/morphos/main_native.cpp`, with `src/platform/morphos/*` (AHI audio, CGX video, bsdsocket / SANA2 networking, native mapper and config GUI, AltiVec video code built with `-maltivec`) linked from `platform/libmorphos*.a`. In this mode `sdlmain.cpp`, `sdl_mapper.cpp`, `sdl_gui.cpp` and `gui_tk` are excluded, and large parts of `render.cpp` are compiled out under `#ifndef __MORPHOS__`.
- Caveat: `MORPHOS_NATIVE` is referenced in `src/Makefile.am`, `src/gui/Makefile.am`, `src/platform/Makefile.am` and `src/libs/Makefile.am`, but it is **not defined in `configure.ac`**, and the `src/platform/morphos/` sources are **not tracked in this repository**. Native-frontend builds will not work from this tree alone; check with the user before assuming either exists.
- `config.h` is committed (hand-maintained for the port), so changes to feature defines may need to be made there as well as in `configure.ac`.
- `dosbox.conf` at the root is the shipped default config for the port.

## WarpOS build

`make -f makefile.wos_gcc` cross-builds a WarpOS/PPC `wos_objs/dosbox.warpelf` (ET_REL, converted on the Amiga with `Elf2Exe2`) using the MiniGL V19 SDK's `Makefile.wos.gcc.common` toolchain settings, the MOS2WOS SDK's WarpOS SDL 1.2 frontend, and `-D__WARPOS__` (config switches in the `__WARPOS__` block of `config.h`). It is independent of autotools. Key constraints, see `src/platform/warpos/README`:
- No C++ exceptions (Elf2Exe2 rejects the runtime's unwind relocations): with `C_NO_EXCEPTIONS`, `E_Exit`/quit `longjmp` to `main()` (`DOSBOX_Quit`, `DOSBOX_TRY`/`DOSBOX_CATCH_ALL` in `include/dosbox.h`). Don't add `throw`/`try`.
- COMDAT `.group` sections are stripped from every C++ object; the link must still pass `WOS_VALIDATE_ETREL`.
- `src/platform/warpos/include/bits/gthr-default.h` overrides the SDK header (its mutex size does not match the prebuilt libstdc++.a); a full clean rebuild is needed after touching it (`-MMD` does not track SDK headers).
- Screen and input are native on WarpOS (`src/platform/warpos/wos_video.c`, `__WARPOS__` blocks in `sdlmain.cpp`); SDL is initialised for audio only. Every PPC->68k OS call costs milliseconds: keep them out of per-tick paths. `WOS_PROF=1` writes a time-share profile to `T:dbprof.txt` (`wos_prof.h`).
- 3dfx Voodoo: core imported from DOSBox-X (`src/hardware/voodoo_emu.cpp`, `voodoo_interface.cpp`, `voodoo_*.h`, commit noted in `voodoo.cpp`), 0.74 glue in `voodoo.cpp`, accelerated output through MiniGL fixed-function GL in `voodoo_minigl.cpp` (input from the MiniGL window via `src/platform/warpos/wos_mglinput.c`).

## Upstream structure (where to look)

`src/cpu` (cores: normal, full, simple, dyn_x86, dynrec), `src/fpu`, `src/hardware` (VGA, SB, GUS, OPL, mixer, PIC/timer, IPX, serial), `src/dos` (DOS kernel and drives), `src/ints` (BIOS/DOS interrupt handlers), `src/shell` (COMMAND.COM and built-in commands), `src/gui` (SDL glue, renderer/scalers, MIDI), `src/debug`, `src/misc`. Headers are in `include/`. Components register themselves through the `Section`/`Module_base` config system (`include/setup.h`) and are initialised from `src/dosbox.cpp`.

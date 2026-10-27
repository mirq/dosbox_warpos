/*
 * Compile MiniGL's Hyperion DLL loader with the Amiga 2-byte struct layout.
 *
 * The DLL side (hyperion_dll/dllstartup.c) and 68k exec expect a packed
 * struct Message (20 bytes, mn_ReplyPort at 14). A client loader built with
 * natural PPC alignment sends malformed messages and MiniGLOpen() dies with a
 * Data Storage exception (DAR=0xC). Same wrapper as quake3amiga's
 * code/amiga/wos_hyperion_loader.c; HYPERION_DLL_C is set by makefile.wos_gcc.
 */
#pragma pack(push,2)

#include <exec/exec.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include HYPERION_DLL_C

#pragma pack(pop)

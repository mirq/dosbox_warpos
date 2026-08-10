SUBDIRS = visualc

EXTRA_DIST = sdl-win32.diff
if MORPHOS_NATIVE
noinst_LIBRARIES = libmorphos.a libmorphos_altivec.a
libmorphos_a_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src/platform/morphos
libmorphos_a_SOURCES = \
	morphos/ahi_backend.cpp \
	morphos/conf_gui.cpp \
	morphos/platform_exec.cpp \
	morphos/video_cgx.cpp \
	morphos/runtime.cpp \
	morphos/net_bsdsocket.cpp \
	morphos/sana2_backend.cpp \
	morphos/mapper_native.cpp
endif

if MORPHOS_NATIVE
libmorphos_altivec_a_CPPFLAGS = -I$(top_srcdir)/include
libmorphos_altivec_a_CFLAGS = -maltivec -fno-tree-vectorize
libmorphos_altivec_a_SOURCES = morphos/video_altivec.c
endif

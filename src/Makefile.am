AM_CPPFLAGS = -I$(top_srcdir)/include

SUBDIRS = cpu debug dos fpu gui hardware libs ints misc shell platform

bin_PROGRAMS = dosbox

if HAVE_WINDRES
ico_stuff = winres.rc
endif

.rc.o:
	$(WINDRES) -o $@ $<

if MORPHOS_NATIVE
dosbox_SOURCES = dosbox.cpp platform/morphos/main_native.cpp $(ico_stuff)
morphos_ldadd = platform/libmorphos.a platform/libmorphos_altivec.a
gui_tk_ldadd =
else
dosbox_SOURCES = dosbox.cpp $(ico_stuff)
morphos_ldadd =
gui_tk_ldadd = libs/gui_tk/libgui_tk.a
endif

dosbox_LDADD = cpu/libcpu.a debug/libdebug.a dos/libdos.a fpu/libfpu.a \
               hardware/libhardware.a gui/libgui.a ints/libints.a misc/libmisc.a \
               shell/libshell.a hardware/mame/libmame.a hardware/serialport/libserial.a \
               $(gui_tk_ldadd) $(morphos_ldadd)

EXTRA_DIST = winres.rc dosbox.ico

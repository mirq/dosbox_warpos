AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_LIBRARIES = libgui.a

common_gui_sources = dosbox_logo.h \
	render.cpp render_scalers.cpp render_scalers.h \
	render_templates.h render_loops.h render_simple.h \
	render_templates_sai.h render_templates_hq.h \
	render_templates_hq2x.h render_templates_hq3x.h \
	midi.cpp midi_win32.h midi_oss.h midi_coreaudio.h midi_alsa.h \
	midi_coremidi.h dosbox_splash.h render_glsl.h

if MORPHOS_NATIVE
libgui_a_SOURCES = $(common_gui_sources)
else
libgui_a_SOURCES = sdlmain.cpp sdl_mapper.cpp sdl_gui.cpp $(common_gui_sources)
endif

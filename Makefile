CC = x86_64-w64-mingw32-gcc
CXX = x86_64-w64-mingw32-g++
WINDRES = windres
CFLAGS = -O2 -Wall -Wextra -DSH_MENU_OVERLAY_ENABLED=1
CXXFLAGS = -O2 -Wall -Wextra -std=c++11 \
           -DSH_MENU_OVERLAY_ENABLED=1 \
           -Ithird_party/imgui -Ithird_party/imgui/backends

ifdef QUIET
CFLAGS += -Wno-cast-function-type -Wno-unused-function \
          -Wno-strict-aliasing -Wno-stringop-truncation
CXXFLAGS += -Wno-cast-function-type -Wno-unused-function \
            -Wno-strict-aliasing -Wno-stringop-truncation
endif

OUTDIR = build
OBJDIR = $(OUTDIR)/obj
IMPLIB = $(OUTDIR)/libscripthook.a

C_SOURCES = loader.c scripthook_api.c scripthook_physics.c \
            scripthook_health.c scripthook_state.c scripthook_entity.c \
            scripthook_spawn.c scripthook_npc.c scripthook_domino.c \
            scripthook_hit.c scripthook_camera.c scripthook_head.c \
            scripthook_fov.c scripthook_modecompat.c scripthook_fp2.c \
            fp2_runtime_state.c scripthook_blur.c scripthook_stat.c \
            scripthook_resource.c scripthook_stealth.c scripthook_ammo.c \
            scripthook_weather.c scripthook_crash.c scripthook_input.c \
            scripthook_havok.c scripthook_reflect.c scripthook_ui.c \
            scripthook_scene.c scripthook_uiprop.c scripthook_uiinput.c \
            scripthook_dinput.c scripthook_hud.c scripthook_menu.c \
            scripthook_lang.c scripthook_config.c scripthook_frame.c \
            scripthook_files.c guard.c \
            third_party/minhook/src/buffer.c \
            third_party/minhook/src/hook.c \
            third_party/minhook/src/trampoline.c \
            third_party/minhook/src/hde/hde64.c

CXX_SOURCES = scripthook_overlay.cpp \
              third_party/imgui/imgui.cpp \
              third_party/imgui/imgui_draw.cpp \
              third_party/imgui/imgui_tables.cpp \
              third_party/imgui/imgui_widgets.cpp \
              third_party/imgui/backends/imgui_impl_dx11.cpp

C_OBJECTS = $(addprefix $(OBJDIR)/,$(C_SOURCES:.c=.o))
CXX_OBJECTS = $(addprefix $(OBJDIR)/,$(CXX_SOURCES:.cpp=.o))
RESOURCE_OBJECT = $(OBJDIR)/ghosthook_overlay_res.o

.PHONY: all docs clean

$(OUTDIR):
	mkdir -p $(OUTDIR)

docs:
	doxygen Doxyfile

all: $(OUTDIR)/dinput8.dll

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(RESOURCE_OBJECT): ghosthook_overlay.rc resource.h assets/ghosthook_logo.png
	@mkdir -p $(@D)
	$(WINDRES) -i $< -o $@

$(OUTDIR)/dinput8.dll: $(C_OBJECTS) $(CXX_OBJECTS) $(RESOURCE_OBJECT) compat_exports.def \
                       scripthook.h image.h log.h fp2_internal.h fp2_timing.h \
                       player_peek.h scripthook_menu_overlay.h | $(OUTDIR)
	$(CXX) -shared -static -static-libgcc -static-libstdc++ -o $@ \
		$(C_OBJECTS) $(CXX_OBJECTS) $(RESOURCE_OBJECT) compat_exports.def \
		-ldinput8 -ldxguid -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 \
		-lole32 -lwindowscodecs \
		-Wl,--out-implib,$(IMPLIB)
	@if objdump -p $@ | grep -Eq 'lib(winpthread|stdc\+\+|gcc)'; then \
		echo "dinput8.dll imports a MinGW runtime: the game cannot load it"; \
		rm -f $@ $(IMPLIB); exit 1; fi

clean:
	rm -rf $(OBJDIR)
	rm -f $(OUTDIR)/*.dll
	rm -f $(OUTDIR)/*.asi
	rm -f $(OUTDIR)/*.a
	rm -f $(OUTDIR)/*.log

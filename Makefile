CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc

ifdef QUIET
CFLAGS += -Wno-cast-function-type -Wno-unused-function \
          -Wno-strict-aliasing -Wno-stringop-truncation
endif

OUTDIR = build
IMPLIB = $(OUTDIR)/libscripthook.a

.PHONY: all docs clean

$(OUTDIR):
	mkdir -p $(OUTDIR)

docs:
	doxygen Doxyfile

all: $(OUTDIR)/dinput8.dll

$(OUTDIR)/dinput8.dll: loader.c scripthook_api.c scripthook_physics.c \
                       scripthook_health.c scripthook_state.c \
                       scripthook_entity.c scripthook_spawn.c \
                       scripthook_npc.c scripthook_domino.c \
                       scripthook_hit.c scripthook_camera.c \
                       scripthook_head.c scripthook_fov.c \
                       scripthook_blur.c \
                       scripthook_stat.c scripthook_resource.c \
                       scripthook_stealth.c scripthook_ammo.c \
                       scripthook_weather.c scripthook_crash.c \
                       scripthook_input.c scripthook_havok.c \
                       scripthook_reflect.c scripthook_ui.c \
                       scripthook_scene.c scripthook_uiprop.c \
                       scripthook_uiinput.c scripthook_dinput.c \
                       scripthook_hud.c scripthook_menu.c scripthook_frame.c guard.c \
                       scripthook.h image.h log.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ loader.c scripthook_api.c \
		scripthook_physics.c scripthook_health.c \
		scripthook_state.c scripthook_entity.c \
		scripthook_spawn.c scripthook_npc.c \
		scripthook_domino.c scripthook_hit.c \
		scripthook_camera.c scripthook_head.c \
		scripthook_fov.c scripthook_blur.c \
		scripthook_stat.c scripthook_resource.c \
		scripthook_stealth.c scripthook_ammo.c \
		scripthook_weather.c scripthook_crash.c \
		scripthook_input.c scripthook_havok.c \
		scripthook_reflect.c scripthook_ui.c \
		scripthook_scene.c scripthook_uiprop.c \
		scripthook_uiinput.c scripthook_dinput.c \
		scripthook_hud.c scripthook_menu.c scripthook_frame.c guard.c \
		-ldinput8 -ldxguid -lgdi32 -luser32 \
		-Wl,--out-implib,$(IMPLIB)
	@if x86_64-w64-mingw32-objdump -p $@ | grep -q libwinpthread; then \
		echo "dinput8.dll imports libwinpthread: the game cannot load it"; \
		rm -f $@ $(IMPLIB); exit 1; fi

clean:
	rm -f $(OUTDIR)/*.dll
	rm -f $(OUTDIR)/*.asi
	rm -f $(OUTDIR)/*.a
	rm -f $(OUTDIR)/*.log
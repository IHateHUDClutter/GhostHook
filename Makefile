CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -shared -static-libgcc

ifdef QUIET
CFLAGS += -Wno-cast-function-type -Wno-unused-function \
          -Wno-strict-aliasing -Wno-stringop-truncation
endif

OUTDIR = build
IMPLIB = $(OUTDIR)/libscripthook.a

.PHONY: all roulette fling tpgun spawner crazycars freecam fov fps \
        chaos sample docs clean

$(OUTDIR):
	mkdir -p $(OUTDIR)

sample: $(OUTDIR)/ui_sample.asi

$(OUTDIR)/ui_sample.asi: ui_sample.c scripthook.h $(OUTDIR)/dinput8.dll | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ ui_sample.c \
		-L$(OUTDIR) -lscripthook -luser32

docs:
	doxygen Doxyfile

all: $(OUTDIR)/dinput8.dll

roulette: $(OUTDIR)/tp_roulette.asi

fling: $(OUTDIR)/hitfling.asi

$(OUTDIR)/hitfling.asi: hitfling.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ hitfling.c -lgdi32 -luser32

freecam: $(OUTDIR)/freecam.asi

$(OUTDIR)/freecam.asi: freecam.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ freecam.c -lgdi32 -luser32

fps: $(OUTDIR)/firstperson.asi

$(OUTDIR)/firstperson.asi: firstperson.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ firstperson.c -lgdi32 -luser32

chaos: $(OUTDIR)/chaos.asi

$(OUTDIR)/chaos.asi: chaos.c scripthook.h $(OUTDIR)/dinput8.dll | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ chaos.c \
		-L$(OUTDIR) -lscripthook -lgdi32 -luser32 -lwinmm

fov: $(OUTDIR)/fov_changer.asi

$(OUTDIR)/fov_changer.asi: fov_changer.c scripthook.h $(OUTDIR)/dinput8.dll | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ fov_changer.c \
		-L$(OUTDIR) -lscripthook -lgdi32 -luser32

spawner: $(OUTDIR)/spawner.asi

$(OUTDIR)/spawner.asi: spawner.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ spawner.c -lgdi32 -luser32

crazycars: $(OUTDIR)/CrazyCars.asi

$(OUTDIR)/CrazyCars.asi: crazycars.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ crazycars.c -lgdi32 -luser32

tpgun: $(OUTDIR)/tpgun.asi

$(OUTDIR)/tpgun.asi: tpgun.c scripthook.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ tpgun.c -lgdi32 -luser32

$(OUTDIR)/tp_roulette.asi: tp_roulette.c scripthook.h $(OUTDIR)/dinput8.dll | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ tp_roulette.c \
		-L$(OUTDIR) -lscripthook -lgdi32 -luser32

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
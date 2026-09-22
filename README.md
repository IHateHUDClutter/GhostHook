# GhostHook

A TU25-compatible Ghost Recon Wildlands ScriptHook fork based on **Phiality / PhialsBasement's
[GRW ScriptHook](https://github.com/PhialsBasement/grw-scripthook)**.
GhostHook loads as a `dinput8.dll` proxy, exposes the ScriptHook plain C ABI,
and loads compatible `.asi` plugins from `GhostHookPlugins/`.

## Beta status and supported builds

The initial GhostHook TU25 beta has completed final release validation and is
ready for publication. The runtime remains frozen; packaging and publication
are the remaining release steps.

Normal GhostHook startup accepts these executable fingerprints and rejects
unsupported timestamp/SizeOfImage combinations:

| GRW executable | PE timestamp | SizeOfImage |
| --- | --- | --- |
| Legacy | `0x6A7C5143` | `0x18B09000` |
| TU25 | `0x6A99768A` | `0x185BA000` |

Build acceptance does not imply identical feature availability on both builds.
The restored frame-callback implementation is currently implemented and
validated on TU25; registration is refused on unsupported builds or a hook
signature mismatch.

## Known compatibility issue

Immersive Healing is temporarily unavailable for the initial GhostHook TU25 beta while a reproducible runtime interaction is investigated.

The remaining current mod set completed a two-hour final stress run without
reproducing the issue. This does not establish universal third-party plugin
compatibility or exhaustive runtime testing of every API.

## Documentation

The current public API is defined by `scripthook.h`. Generate its reference
with `make docs`; output is generated locally in `docs/api/` and is not tracked.
The [upstream API reference](https://phialsbasement.github.io/grw-scripthook/api/)
is useful background but may differ from the current GhostHook header.

[docs/plugins.md](docs/plugins.md) is the plugin author's guide:
how loading works, which threads call you, and the rules that keep
a plugin from crashing the game. [docs/ui.md](docs/ui.md) covers
the native UI: scenes, widgets, properties, input and reloads.

## What works

| Capability | Notes |
| --- | --- |
| Vehicle spawning | 65 vehicles, catalogued and named by hand |
| Entity enumeration | Kind, position, health, components |
| Entity visibility | Hide or show anything, optionally held |
| Teleport | Verified 9.9km cross map, lands within 3m |
| Health | Read and write through the game's obfuscated storage |
| Ground queries | Uses the engine's own collision world |
| Camera | Position, orientation, roll, fov, the view matrices |
| OnHit events | Entity, position, normal, distance, shooter |
| OnFire events | Muzzle, direction, yaw and pitch, shooter |
| Native UI | The engine's own widgets, scenes per plugin, keys with focus |
| Menu | One shared root, plugins add submenus |
| Overlay | Slots that pack themselves, drawn by the game |
| Game state | Menu, loading, in game, transitions |

The source repository includes these example mods; the runtime package does
not bundle mods:

- **hitfling** shoot a car, it launches into the air
- **tpgun** shoot anywhere, you arrive there
- **ui_sample** a window from the UI ABI alone: its own scene,
  rows, a highlight bar, keys through the input callback, a
  rebuild after a world reload (F7 toggles it)

## Building from source

Use a MinGW-w64 x64 compiler and Make, for example in MSYS2 MINGW64.
From the public repository root:

```sh
make -B QUIET=1 build/dinput8.dll
```

The production DLL is written to `build/dinput8.dll`. The build also generates
`build/libscripthook.a`, the developer import library. Output is not installed
into the game directory automatically. The explicit `all` target builds the
production DLL; bare `make` currently selects the build-directory target.

`make sample` builds the native UI example. `make docs` generates the API
reference from the public header and guides using Doxygen.

## Installing and using GhostHook

1. Copy GhostHook's `dinput8.dll` beside `GRW.exe`.
2. Launching GhostHook automatically creates `GhostHookPlugins/` if missing.
3. Put GhostHook-compatible `.asi` plugins inside `GhostHookPlugins/`.
4. Root-level `.asi` files are intentionally ignored.
5. After initialization, press F4 to open or close the GhostHook menu.

Typical layout:

```text
Ghost Recon Wildlands/
    GRW.exe
    dinput8.dll
    GhostHookPlugins/
        ExampleMod.asi
        ExampleMod.ini
```

GhostHook automatically opens its menu after the initial in-game initialization
completes. Arrow keys navigate, Enter selects/confirms, and Backspace returns.
Menu-owned keys are suppressed while needed without broadly locking normal
keyboard input.

GhostHook does not relocate or centrally manage plugin INI files. Most current
plugins resolve their INI beside their own ASI, as in the example above.
Individual plugins may use a different path according to their implementation;
follow each plugin's instructions.

## Writing a mod

A plugin is a DLL named `.asi`. The loader runs plugins from a
worker thread rather than from `DllMain`, so a plugin can link
`build/libscripthook.a` and call the API directly, or resolve it through
`GetProcAddress` to also run on older loaders. The guide in
[docs/plugins.md](docs/plugins.md) walks through both.

The whole of the falling cars mod:

```c
static void OnHit(const ShHit *hit, void *user) {
    ShVec3 up = hit->pos;
    if (hit->kind != SH_KIND_VEHICLE) return;
    up.z += 90.0f;
    ShPlaceEntity(hit->root, &up, NULL);
}

while (!ShIsInGame() || !ShHitHookInstall()) Sleep(500);
ShOnHit(OnHit, NULL, 0);
```

Receivers are called on a worker thread the API owns. Follow each API
function's documented requirements when calling it from a receiver. `hit->root` is already resolved for you,
because bullets usually strike a child part rather than the vehicle.

## The ABI

GhostHook preserves the existing public ScriptHook API/ABI.
`scripthook.h` is the authoritative public header. `SH_API_VERSION` remains 1,
and the current GhostHook DLL exports all 240/240 declared public functions.
Return values and error behavior are documented per function in that header.
The import library is developer-facing; ordinary users do not need it.
See the plugin guide for TU25 frame callbacks and their threading limits.

```
player        ShGetPlayer ShGetPlayerPosition ShTeleportPlayer
              ShTeleportPlayerHops ShTeleportPlayerToGround
              ShIsInVehicle ShWalkToRoot

entities      ShFindEntities ShGetEntityKind ShKindName
              ShPlaceEntity ShGetComponents ShFindComponent
              ShSetEntityVisible ShEntityNodeCount

camera        ShGetCamera ShSetCamera ShCameraOrbit
              ShCameraFree ShCameraAngles ShCameraApply
              ShCameraMatrix ShCameraRelease

menu          ShMenuCreate ShMenuSub ShMenuAction
              ShMenuToggle ShMenuNumber ShMenuList
              ShMenuStatus ShMenuSetKey ShMenuOpen

overlay       ShHudCreate ShHudSet ShHudColour
              ShHudShow ShHudDestroy

vehicles      ShSpawnVehicle ShVehicleCount ShVehicleAt
              ShVehicleName

health        ShGetHealthPlayer ShSetHealthPlayer
              ShSetGodModePlayer ShSetCannotDiePlayer
              ShGetHealthEntity ShSetHealthEntity

events        ShHitHookInstall ShOnHit ShOffHit ShGetHits
              ShOnFire ShOffFire ShGetShots

physics       ShPhysicsReady ShGroundHeight ShGroundHeightFrom
              ShRayLog ShQueryRays ShRayFilterPlayer

engine        ShQueueCall ShQueueResult ShGetGameState ShIsInGame

ui            ShUiSceneCreate ShUiSceneSetOrder ShUiSceneShow
              ShUiSceneDestroy ShUiCreateIn ShUiReparent
              ShUiChildCount ShUiChildAt ShUiDestroy
              ShUiSetF ShUiSetU ShUiSetV ShUiSetS ShUiGetF
              ShUiGetU ShUiGetV ShUiGetS ShUiMeasure
              ShUiSetAutoSize ShUiBegin ShUiCommit
              ShUiCommitAsync ShUiSetReset ShUiSetInput
              ShUiFocus ShUiTextureCreate ShUiSetDefaultFont
```

### Events

`ShOnHit(fn, user, flags)` delivers one event per bullet, at the
impact that stopped it. Flags are opt in:

- `SH_EVT_MINE_ONLY` only your own shots
- `SH_EVT_NO_SELF` drop the graze on the firer's own body

A projectile is stepped every frame and its hit list accumulates, so
the API holds each bullet and reports its furthest hit once the
projectile stops being stepped. That costs about 120ms of latency
and is why acting on the first reported hit puts you on a fence post
instead of the target.

### Threads

Engine calls that take locks deadlock from any thread but the game
thread. `ShQueueCall` runs a call there and `ShQueueResult` collects
it, usually on the next frame. The API uses this internally, so
plugins rarely need it.

## Hazards

These are real, and each one cost a crash to find.

- Two vehicles freeze the game if you enter them: an alpaca that the
  engine classes as a vehicle, `0x40081214`, and an unused monster
  truck, `0x40BA6E9D`. They are safe to spawn, not to ride.
- Placing an entity outside the streamed region crashes the game.
  Collision only exists within roughly 1500m of the player.
- `ShPlaceEntity` carries riders on purpose, so moving a vehicle you
  are sitting in takes you with it.

## Addressing

Every engine address is stored as an RVA and resolved against the
module base at runtime, so ASLR is fine. `image.h` holds the helper.
The game currently loads at its preferred base under Proton, which
made pinned addresses easy to get away with and easy to get wrong.

## Layout

```
loader.c              dinput8 proxy, loads the real DLL and the .asi files
scripthook_api.c      player, teleport, entity placement, errors
scripthook_entity.c   enumeration, components, kinds, visibility
scripthook_health.c   the obfuscated health storage
scripthook_state.c    game flow state, pause detection
scripthook_physics.c  ray hook, ground queries, game thread queue
scripthook_spawn.c    vehicle catalogue and spawning
scripthook_hit.c      OnHit and OnFire
scripthook_camera.c   the camera hook, per field ownership
scripthook_head.c     the head bone, position and orientation
scripthook_fov.c      field of view, taken at its source
scripthook_blur.c     the hidden close range blur, one byte
scripthook_stat.c     the obfuscated stat storage
scripthook_resource.c resources and skill points
scripthook_stealth.c  detection visibility scale
scripthook_ammo.c     ammo by weapon slot
scripthook_weather.c  weather type and time of day
scripthook_reflect.c  method tables by name, scenes, GameFlow objects
scripthook_ui.c       native widgets, panels, labels, quads
scripthook_scene.c    the phoenix scene of our own that hosts them
scripthook_uiprop.c   widget properties by id from the engine's tables
scripthook_uiinput.c  keys and pointer for the focused scene
scripthook_dinput.c   the DirectInput keyboard wrapper, blocked keys
scripthook_input.c    cursor freeze and the modifier poll stub
scripthook_hud.c      overlay slots
scripthook_menu.c     the shared F4 menu
guard.c               landing pad for the spawn trampoline

hitfling.c tpgun.c    the example mods
ui_sample.c           the native UI example
```

## Credits / Upstream

GhostHook is based on Phiality / PhialsBasement's
[GRW ScriptHook](https://github.com/PhialsBasement/grw-scripthook).
The upstream implementation and research form the basis of this fork.

The camera work stands on **Firejumper93's**
[GhostReconWildlandsVR](https://github.com/Firejumper93/GhostReconWildlandsVR),
MIT licensed and unusually well documented. Its notes gave us the
camera struct layout, the fact that `Camera+0x000` is the transform
the view builder actually consumes rather than one of the derived
matrices, and this engine's yaw and pitch convention. Their build log
also records the write to `+0x4A0` that quietly does nothing, which
is exactly the wrong turn we would have taken.

The addresses here are our own, since this build is newer than any in
their table, but the reverse engineering that made them meaningful is
theirs.

## Licence

GPL-3.0. See `LICENSE`.

You are free to use, modify and redistribute this, including
commercially. What you cannot do is take it, add features, and ship
that as a closed product: any derivative has to be released under
the GPL with its source available. Sell builds if you like, but the
improvements come back to everyone.

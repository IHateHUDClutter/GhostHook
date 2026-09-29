# GhostHook

A TU25-compatible Ghost Recon Wildlands ScriptHook fork based on **Phiality / PhialsBasement's
[GRW ScriptHook](https://github.com/PhialsBasement/grw-scripthook)**.
GhostHook loads as a `dinput8.dll` proxy, exposes the ScriptHook plain C ABI,
and loads compatible `.asi` plugins from `GhostHookPlugins/`.

## Beta status and supported builds

The GhostHook TU25 beta includes the validated native-menu startup update.
The public ScriptHook API/ABI remains compatible with existing plugins.

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

## Native menu startup

Native menu startup has been significantly improved. Repeated menu
initialization delays have been reduced, and startup is substantially more
consistent. The first launch after installing or updating GhostHook may take
longer than normal for the menu to appear. Subsequent launches should
initialize faster and more consistently.

## GhostHook Settings

GhostHook includes a built-in GhostHook Settings submenu.

Menu Scaling options:

- Small — 0.85x
- Default — 1.00x
- Large — 1.25x
- XLarge — 1.50x

The selected menu scale is saved in `GhostHook.ini` beside `dinput8.dll` and
restored automatically when the game is launched again.

## Reforged Compatibility

GhostHook includes compatibility support for several Reforged mods, including
First Person, Skip Intro Videos, Vehicle Dispatch, and FOV Changer.

## Menu Ordering

Mod entries in the GhostHook menu are listed alphabetically. GhostHook Settings
remains the final entry in the main menu.

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
| Teleport | Cross-map player teleport support |
| Health | Read and write through the game's obfuscated storage |
| Ground queries | Uses the engine's own collision world |
| Camera | Position, orientation, roll, fov, the view matrices |
| OnHit events | Entity, position, normal, distance, shooter |
| OnFire events | Muzzle, direction, yaw and pitch, shooter |
| Native UI | The engine's own widgets, scenes per plugin, keys with focus |
| Menu | One shared root, plugins add submenus |
| Overlay | Slots that pack themselves, drawn by the game |
| Game state | Menu, loading, in game, transitions |

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

`make docs` generates the API
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

For example, a vehicle hit callback can move the resolved vehicle root:

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
and the current GhostHook DLL exports the functions declared by that header.
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
projectile stops being stepped. This introduces some reporting latency
and avoids acting on an earlier intermediate hit.

### Threads

Engine calls that take locks deadlock from any thread but the game
thread. `ShQueueCall` runs a call there and `ShQueueResult` collects
it, usually on the next frame. The API uses this internally, so
plugins rarely need it.

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
```

## Credits / Upstream

GhostHook is based on Phiality / PhialsBasement's
[GRW ScriptHook](https://github.com/PhialsBasement/grw-scripthook).
The upstream implementation and research form the basis of this fork.

The camera implementation also draws on **Firejumper93's**
[GhostReconWildlandsVR](https://github.com/Firejumper93/GhostReconWildlandsVR)
research. GhostHook retains attribution for that contribution.

Comparative work in [GameXueRen's GRW ScriptHook fork](https://github.com/GameXueRen/grw-scripthook)
provided useful compatibility and implementation leads. GhostHook independently
integrated and adapted the relevant concepts.

## Licence

GPL-3.0. See `LICENSE`.

You are free to use, modify and redistribute this, including
commercially. What you cannot do is take it, add features, and ship
that as a closed product: any derivative has to be released under
the GPL with its source available. Sell builds if you like, but the
improvements come back to everyone.

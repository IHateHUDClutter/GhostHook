# GhostHook v1.0

GhostHook is a Ghost Recon Wildlands ScriptHook fork/runtime. It loads as
an x64 `dinput8.dll` proxy, exposes a plain C ScriptHook ABI, and loads
compatible `.asi` plugins from `GhostHookPlugins`.

Validated through testing.

## Supported builds and safety

GhostHook recognizes the supported Legacy and TU25 executable builds.
Unsupported builds fail closed: build-specific initialization is rejected.
Build acceptance does not imply identical feature availability on both builds;
individual features may require additional build and signature checks.

The public API remains defined by `scripthook.h`, with `SH_API_VERSION` set
to 1.

## Shared overlay menu

The shared `ShMenu*` menu renders through D3D11 and Dear ImGui.
Existing compatible plugins do not require recompilation merely because
the menu renderer changed.

Plugin entries are alphabetical, with GhostHook Settings always last.
The menu supports larger plugin collections, nested submenus, an expanded
shared page pool, and lists with up to 64 choices. A bounded viewport follows
the selected row in both the root menu and submenus.

Native `ShUi*` widgets and `ShHud*` HUD interfaces remain available.
They are separate game-native systems, not the shared overlay menu.

### Menu controls

| Key | Action |
| --- | --- |
| F4 | Open or close the shared menu |
| Arrow keys | Navigate rows and change values |
| Enter | Select or confirm |
| Backspace | Return to the previous menu |

Menu-owned keys are suppressed while needed without broadly locking normal
keyboard input. By default, the menu opens after initial in-game
initialization; this can be disabled in GhostHook Settings.

## GhostHook Settings

Settings are saved automatically in `GhostHook.ini` beside `dinput8.dll`
and restored on later launches. Existing valid saved choices are retained.
Missing or invalid values use the corresponding defaults.

`GhostHook.ini` is user-generated and is not included in the release package.

| Setting | Choices |
| --- | --- |
| Menu Scaling | Small — 0.85x; Default — 1.00x; Large — 1.25x; XLarge — 1.50x |
| Font | Segoe UI, Arial, Tahoma, Verdana, Trebuchet MS, Georgia |
| Font Size | 16, 18, 20, 22, 24, 26, 28, 30; default **26** |
| Text Color | White, Light Gray, Green, Cyan, Yellow, Orange, Red, Custom |
| Background Opacity | 0%, 10%, 20%, 30%, 40%, 50%, 60%, 70%, 80%, 90%, 100%; default **90%** |
| Accent Color | Default, White, Light Gray, Green, Cyan, Yellow, Orange, Red, Custom |
| Open Menu on Startup | Yes or No; default **Yes** |

The Default accent preset uses exactly **RGB 58, 80, 96**.
Custom text and accent colors retain independently configurable RGB
components from 0 to 255.

Background opacity affects only the main panel background. It does not fade
the logo, text, selected-row highlight, or status/footer. At 0%, the panel
background is fully transparent; at 100%, it is fully opaque.

Font choices use installed system fonts. No font files are redistributed.

## Plugin compatibility

GhostHook provides broader plugin compatibility through reusable loader/API
compatibility additions and supports plugins targeting compatible ScriptHook
interfaces.

Compatibility depends on each plugin's requirements, including its API
usage, executable assumptions, and external assets. This is not a guarantee
that every ScriptHook plugin will work. Follow each plugin's installation
and configuration instructions.

## Installing and using GhostHook

1. Close the game before installing or updating.
2. Back up any existing `dinput8.dll` before replacing it.
3. Place GhostHook's `dinput8.dll` beside `GRW.exe`.
4. Place compatible `.asi` plugins directly inside `GhostHookPlugins`.
5. Launch the game and use F4 to open or close the shared menu.

Required layout:

```text
Ghost Recon Wildlands\
    GRW.exe
    dinput8.dll
    GhostHookPlugins\
        ModName.asi
```

GhostHook creates `GhostHookPlugins` if it is missing. Root-level `.asi`
files are intentionally ignored.

GhostHook does not relocate or centrally manage plugin INI files. Plugins
may keep configuration beside their ASI or use another path of their own.
Follow the individual plugin's instructions rather than moving all
configuration files into a common location.

## Documentation

The current public API is defined by [scripthook.h](scripthook.h).
Generate its reference with `make docs`; output is generated locally in
`docs/api/` and is not tracked.

The [upstream API reference](https://phialsbasement.github.io/grw-scripthook/api/)
is useful background but may differ from the current GhostHook header.
The current header takes precedence.

- [Plugin author's guide](docs/plugins.md): loading, linking, callbacks,
  threading, and plugin safety.
- [Native UI guide](docs/ui.md): game-native scenes, widgets, properties,
  input, and reloads. This guide's native UI material concerns `ShUi*`;
  the current shared `ShMenu*` renderer is the overlay described above.

## Capabilities

Availability depends on the supported build and each function's documented
requirements.

| Capability | Notes |
| --- | --- |
| Vehicle spawning | Named vehicle catalogue and spawning interfaces |
| Entity enumeration | Kind, position, health, and components |
| Entity visibility | Hide or show entities, with held visibility controls |
| Teleport | Cross-map player teleport support |
| Health | Read and write through the game's obfuscated storage |
| Ground queries | Uses the engine's collision world |
| Camera | Position, orientation, roll, field of view, and view matrices |
| OnHit events | Entity, position, normal, distance, and shooter |
| OnFire events | Muzzle, direction, yaw, pitch, and shooter |
| Native UI | Engine widgets, per-plugin scenes, and focused input through `ShUi*` |
| Shared menu | Overlay-rendered root, plugin submenus, and selection-following scrolling |
| Native HUD | Game-rendered HUD slots through `ShHud*` |
| Game state | Menu, loading, in-game state, and transitions |

## Building from source

Use a MinGW-w64 x64 toolchain with GCC, G++, Make, and `windres`, for
example in MSYS2 MINGW64. The build requires the Windows and DirectX headers
and import libraries used by the Makefile, including D3D11 and DXGI.
Dear ImGui and MinHook sources are included under `third_party/`.

From the public repository root:

```sh
make -B QUIET=1 build/dinput8.dll
```

The DLL is written to `build/dinput8.dll`. The build also generates
`build/libscripthook.a`, the developer import library. Build output is
not installed into the game automatically.

Use the explicit DLL target above or `make all`; bare `make` currently
selects the build-directory target.

With Doxygen installed, `make docs` generates the API reference from the
public header and guides. Retain applicable third-party licenses when
redistributing source or binaries.

## Writing a mod

A plugin is an x64 DLL named `.asi`. GhostHook initiates plugin loading
from a worker thread rather than from its own `DllMain`. A plugin's
`DllMain` still runs under the Windows loader lock: keep it minimal and
perform substantial initialization outside it.

Include `scripthook.h` and link `build/libscripthook.a`, or resolve API
functions through `GetProcAddress` and handle unavailable functions.
The [plugin guide](docs/plugins.md) covers both approaches.

Example MinGW-w64 link command:

```sh
x86_64-w64-mingw32-gcc -O2 -shared -o myplugin.asi myplugin.c -Lbuild -lscripthook
```

For example, a vehicle-hit receiver can move the resolved vehicle root.
This fragment shows the callback and its registration; the initialization
statements belong in the plugin's initialization routine, outside `DllMain`:

```c
static void OnHit(const ShHit *hit, void *user) {
    ShVec3 up = hit->pos;
    if (hit->kind != SH_KIND_VEHICLE) return;
    up.z += 90.0f;
    ShPlaceEntity(hit->root, &up, NULL);
}

/* Plugin initialization, outside DllMain. */
while (!ShIsInGame() || !ShHitHookInstall()) Sleep(500);
ShOnHit(OnHit, NULL, 0);
```

Hit receivers run on a worker thread owned by the API. Follow each
function's documented requirements when calling it from a receiver.
`hit->root` is already resolved because bullets often strike a child
part rather than the vehicle root.

## Public API and ABI

GhostHook exports the public functions declared by the current
`scripthook.h`. `SH_API_VERSION` remains 1. The header is authoritative
for signatures, structures, return values, and error behavior.

The import library is for developers; ordinary users do not need it.
Check each function's return contract rather than assuming every integer
result is a Boolean. Use `ShLastError` and `ShErrorString` where documented.

Representative API families:

```text
player        ShGetPlayer ShGetPlayerPosition ShTeleportPlayer
              ShTeleportPlayerHops ShTeleportPlayerToGround
              ShIsInVehicle ShWalkToRoot

entities      ShFindEntities ShGetEntityKind ShKindName
              ShPlaceEntity ShGetComponents ShFindComponent
              ShSetVisible ShEntityNodeCount

camera        ShGetCamera ShSetCamera ShCameraOrbit
              ShCameraFree ShCameraAngles ShCameraApply
              ShCameraMatrix ShCameraRelease

menu          ShMenuCreate ShMenuSub ShMenuAction
              ShMenuToggle ShMenuNumber ShMenuList
              ShMenuStatus ShMenuSetKey ShMenuOpen

native HUD    ShHudCreate ShHudSet ShHudColour
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

native UI     ShUiSceneCreate ShUiSceneSetOrder ShUiSceneShow
              ShUiSceneDestroy ShUiCreateIn ShUiReparent
              ShUiChildCount ShUiChildAt ShUiDestroy
              ShUiSetF ShUiSetU ShUiSetV ShUiSetS ShUiGetF
              ShUiGetU ShUiGetV ShUiGetS ShUiMeasure
              ShUiSetAutoSize ShUiBegin ShUiCommit
              ShUiCommitAsync ShUiSetReset ShUiSetInput
              ShUiFocus ShUiTextureCreate ShUiSetDefaultFont
```

### Events

`ShOnHit(fn, user, flags)` delivers one event per bullet at the impact
that stopped it. Flags are opt-in:

- `SH_EVT_MINE_ONLY`: receive only the player's own shots.
- `SH_EVT_NO_SELF`: exclude a graze on the firer's own body.

A projectile is stepped every frame and its hit list accumulates. The API
holds each bullet and reports its furthest hit once the projectile stops
being stepped. This introduces reporting latency and avoids acting on an
earlier intermediate hit.

### Callbacks and threading

Follow the requirements of each API rather than assuming all callbacks
have the same execution context.

- Hit and fire receivers run on an API-owned worker thread.
- Menu callbacks run synchronously with the menu lock released. Keep them
  responsive; they are not an asynchronous work queue.
- Frame callbacks execute synchronously on the engine path that invokes
  them. The executing engine thread can vary. Keep callbacks short and
  nonblocking, and do not assume every API can safely be called there.
- Removing a frame callback is not a completion barrier. Do not free its
  data or unload its code while a callback may still be running.

Frame callback registration is build-qualified and can fail when the build,
signature, or hook requirements are not met. See the plugin guide for
the supported TU25 path and registration/removal details.

Raw engine calls that acquire engine locks can deadlock on the wrong
thread. Use the documented API wrappers; where appropriate, `ShQueueCall`
queues a call for the game thread and `ShQueueResult` retrieves its result.

## Source layout

```text
loader.c                    dinput8 proxy and plugin loading
scripthook.h                public API declarations and contracts
image.h                     build-aware address helpers
scripthook_api.c            player, teleport, entity placement, errors
scripthook_entity.c         enumeration, components, kinds, visibility
scripthook_health.c         obfuscated health storage
scripthook_state.c          game flow state and pause detection
scripthook_physics.c        ray hook, ground queries, game-thread queue
scripthook_spawn.c          vehicle catalogue and spawning
scripthook_hit.c            OnHit and OnFire
scripthook_frame.c          frame callbacks
scripthook_camera.c         camera hook and per-field ownership
scripthook_head.c           head bone position and orientation
scripthook_fov.c            field of view
scripthook_blur.c           close-range blur
scripthook_stat.c           obfuscated stat storage
scripthook_resource.c       resources and skill points
scripthook_stealth.c        detection visibility scale
scripthook_ammo.c           ammunition by weapon slot
scripthook_weather.c        weather type and time of day
scripthook_reflect.c        method tables, scenes, GameFlow objects
scripthook_ui.c             native widgets, panels, labels, quads
scripthook_scene.c          native UI scene hosting
scripthook_uiprop.c         native widget properties
scripthook_uiinput.c        focused-scene keyboard and pointer input
scripthook_dinput.c         DirectInput keyboard wrapper and blocked keys
scripthook_input.c          cursor and input handling
scripthook_hud.c            native HUD slots
scripthook_menu.c           shared menu model and navigation
scripthook_menu_overlay.h   internal menu/renderer bridge
scripthook_overlay.cpp      D3D11/Dear ImGui menu renderer
scripthook_config.c         persistent GhostHook settings
guard.c                    spawn trampoline support
third_party/               bundled dependencies and license notices
```

## Credits and attribution

GhostHook is based on Phiality / PhialsBasement's
[GRW ScriptHook](https://github.com/PhialsBasement/grw-scripthook).
The upstream implementation forms the basis of this fork.

The camera implementation also draws on Firejumper93's
[GhostReconWildlandsVR](https://github.com/Firejumper93/GhostReconWildlandsVR).
GhostHook retains attribution for that contribution.

The overlay uses Dear ImGui and MinHook. Their license and attribution
notices are retained under `third_party/`.

## License

GPL-3.0. See [LICENSE](LICENSE).

Redistribution and modifications must comply with that license.
Bundled third-party components retain their applicable licenses and
copyright notices; preserve those notices when redistributing them.

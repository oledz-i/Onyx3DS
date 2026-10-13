# ONYX 3DS — status (Oct 3, 2026 checkpoint)

## What works and is verified
- `shell/` (game library, 3DS header/icon parsing, settings, per-game overrides,
  folder remapping, cheat database client, SteamGridDB, themes): builds and passes 33 unit tests (188 assertions) under ASan/UBSan.
- Azahar patches (`patches/azahar`, `patches/dynarmic`): UWP file access through
  *FromApp APIs, USB folder remapping hook, static libretro core, timeline-semaphore
  opt-in, working libretro cheat toggling, W^X JIT for Xbox. Syntax-checked for
  Linux, Windows desktop and UWP.
- Mesa/Dozen patch (`patches/mesa`): builds Dozen without LoadLibrary for UWP.
  Syntax-checked for desktop and UWP.
- App non-page code (Vulkan->D3D12 bridge, presenter, audio, input, emulator
  session, services, UI kit): compiles against real C++/WinRT headers (MinGW check).
- Themes (5), menu music (3 original tracks), UI sounds, logos: generated, included.

## Not yet verified
- XAML pages: last check pass found member-vs-type name clashes (FontFamily, Style,
  CornerRadius). A fix was applied but not re-checked; SettingsPage/EmulationPage/
  FolderPage/App results were still pending when the session stopped.
- Nothing has been built with MSVC or run on an Xbox yet. The biggest unknowns:
  1. Dozen building and loading inside a UWP app on Xbox (patches/mesa + build-dozen.ps1)
  2. Azahar compiling as WindowsStore (expect a few more UWP API fixes)
  3. Real 30-60 FPS on Series S — measured only once it runs.

## Crash reporting (Oct 4)
- Native faults, failed Azahar ASSERTs, uncaught C++ exceptions and abort() are now
  written to onyx.log as `[CRASH]` lines with the faulting module+offset and a stack
  scan. Offsets inside ONYX3DS.exe decode with `out/ONYX3DS.map` (in the CI artifact).
- Memory use vs. the Xbox limit is logged at launch and every 500 ms for the first
  15 s of a game, so an out-of-memory kill is visible.
- Azahar logs synchronously now (patch 0008), so its last lines survive a crash.

## Renderer (Oct 4)
- Default renderer is Azahar's software renderer: frames come back as CPU pixels and
  are uploaded to the D3D12 presenter, so Dozen is never used. Hardware (Vulkan via
  Dozen) is opt-in under Settings > Renderer. On the Series S, Dozen lost the device
  during renderer start-up (see onyx.log history); DRED breadcrumbs and the
  device-removed reason are now logged for when that is picked up again.
- A crash with the hardware renderer switches the setting back to Software; a crash
  in JIT code switches the CPU to the interpreter.

## Next steps
1. Re-run the page syntax checks; fix what remains.
2. Push to GitHub and let `.github/workflows/build.yml` build on Windows; fix MSVC errors.
3. Install the .msix via Device Portal, set App type to Game, open Settings > System check.

## v2.1.1 (in progress)
- Hardware renderer boots and runs on Series S (NSMB2 at 60 fps with audio, build #69+).
  Fixed so far: device removal (mesa/0011 cast guard), Close E_FAIL from
  SetViewInstanceMask without view instancing (mesa/0013).
- Black hardware picture (builds #69-#71): readback proved the output image was
  black. Cause: Azahar's present pipeline blends with CONSTANT_ALPHA and constants
  {0,0,0,1}; without options13.AlphaBlendFactorSupported Dozen used BLEND_FACTOR,
  i.e. RGB 0 → every screen draw kept the destination. mesa/0015 broadcasts the
  alpha constant for such pipelines. Self-test steps 7/8 reproduce the present draw.
- Crash after ~1 min: D16 depth images were R16_UNORM + ALLOW_DEPTH_STENCIL (needs
  castable formats). mesa/0015 makes depth images typeless without relaxed casting.
- ~500 ms stalls every ~2.5 s: dynarmic W^X re-protected the whole code buffer per
  block; dynarmic/0003 only toggles the pages being written.
- Build #72 (6ac43864): picture works. Remaining: ~500 ms stall every ~2 s (hardware
  only; main thread in win32u, i.e. a kernel graphics call) and 0.4-0.7 s freezes per
  new pipeline (dzn variant compiled at first draw). Crash after ~90 s near
  "Vertex buffer size exceeds available space" spam (out-of-bounds vertex copy).
  Next build: mesa/0017 (variant precompile, no dedicated allocations, upload buffer
  reuse, rarer trace checks), azahar/0034 (bounded vertex copy), no log flushes,
  custom textures only with a pack, frame rate lock, and "Profile stall" stacks
  (proper unwinding) for any frame over 100 ms.
- Build #73 stall stacks: 96% of stalled-frame samples in VulkanHost::ShowReadback
  waiting on the previous frame's fence (GPU ~0.5 s late), right after
  RasterizerCache::RunGarbageCollector -> vmaDestroyImage -> D3D12 kernel call.
  Next build: azahar/0035 skips small draws while their pipeline builds on Dozen; mesa/0018 skips draws for refused
  pipeline variants (E_INVALIDARG crash); readback shows finished frames without waiting.
- Build #75: the texture recycler corrupted screens (stale views showing other
  surfaces) and did not stop the stall; removed. Stall stacks then pointed at fence
  waits (WaitSyncIndex, MasterSemaphore) ~480 ms every ~2 s, and the presenter
  thread stalls at the same moments in software mode too -> completion events
  arriving late. mesa/0019 + presenter poll fence values while waiting; presenter
  idles under the XAML view; XAML view updates on CompositionTarget.Rendering;
  azahar/0036 frees images on a background thread; mesa/0020 bigger CBV/SRV/UAV
  pool heaps; shader caches remapped to the USB System folder. Navy background
  reverted (azahar/0033 dropped).
- Build #76 (6ac46db0): screens corrupted (top screen drawn over bottom-screen
  content): prefersDedicated=false let D3D12 placed images alias. Reverted in
  mesa/0021. No "fence completed before its event" logs, so the GPU really finishes
  ~470-500 ms late, every ~90 frames, in every hardware log since #69 (about 2/3 of
  60-frame windows), with no pipeline compiles; the vkworker thread is idle then and
  the D3D12 presenter kept presenting at 60/s in swap-chain builds, so only the
  Vulkan queue stalls. 16 pipelines refused with a zero variant key (E_INVALIDARG)
  in lit scenes. Next build: azahar/0037 timestamps every core submission and logs
  stream-buffer wraps; azahar/0038 disables geometry shaders on Dozen; mesa/0022
  logs the stages and DXIL validation verdict of a refused pipeline.
- Build #78: mesa/0023 keeps DEFAULT-heap internal buffers (triangle-fan index
  rewrites, format-changing copies) across command buffer resets instead of a
  CreateCommittedResource + release per use, and logs a 5 s summary of every GPU
  memory call (count, total and max time) plus fan conversions, to test whether
  kernel memory mapping lines up with the stalls.
- Build #79: OpenGL hardware renderer (GlHost: window-less WGL context on the emulation
  thread, RGBA8 FBO, PBO readback). The prebuilt Mesa GL runtime and a patch for it came
  from a third-party project whose author asked us not to use it, so both were removed in
  v3.0.3 (vendor/mesa-gl, old azahar/0039). OpenGL is not offered until we have our own
  Mesa GL-on-D3D12 build; saved OpenGL settings now mean Vulkan. GlHost.cpp is our own
  code and is kept dormant. azahar/0040 clears game_loaded in retro_unload_game.
- Build #81 test (39968e73): OpenGL runs well but stalls exactly like Vulkan: GlHost
  waits ~450 ms for a 3-frame-old fence every ~2.1 s, so the GPU is late on both
  drivers. Every stalling log shows frames through the XAML WriteableBitmap (7.4 MB
  hardware frames; software frames are 20x smaller and stall far less), plus an 8 MB
  step memory sawtooth. Build #82: "Direct display" toggle (qol.direct_display) shows
  CPU frames through the presenter's swap chain instead (alpha mode IGNORE now, in
  case premultiplied alpha was why the panel stayed black in #35); mirror copy only
  for the XAML view; swap chain upload double-buffered with no per-frame CPU wait;
  XAML frame gap / update time logged; GL output FBO sized to the base geometry
  (was 7200x4800). WGL can't enable glthread; the D3D12 GL driver already uses
  Gallium's threaded context.
- Build #85 (major): Direct display is the default and CPU uploads skip the vsync-bound fence wait (lifts the 40 fps cap); azahar/0041 waits a bounded time (160 ms/s) for Dozen pipelines instead of skipping draws; Azahar configured with CMAKE_SYSTEM_PROCESSOR=AMD64 (SSE4.2 paths); stable committed sideload signing key and per-run package version so updates install over the old build; saves mirrored to <USB>/ONYX3DS/SaveBackup after each game and restored on a fresh install; NES (FCEUmm) as a second system.
- Build #88: from the #87 logs. OpenGL gameplay at 30 fps = main thread 56% inside Mesa and 14% memcpy, shader log showed a geometry shader built with every lit shader -> azahar/0042 drops the fixed geometry shader on Mesa D3D12 (same as Vulkan/0038). Vulkan 40 fps menus = emulation thread waiting in the presenter's upload fence -> frames are dropped rather than waited for (3 upload buffers). azahar/0043: pipeline workers hw-1 (max 6) at below-normal priority.
- v3.0.0 (build #89) tester notes, no logs (Oct 6 evening): NES is rough: colours too saturated (FCEUmm default palette; try another palette such as asqrealc/nintendo-vc), Super Mario Bros shows odd graphical glitches and hitches (check frame pacing at 60.1 fps, dropped uploads, FCEUmm options). 3D Land runs quite well on OpenGL with minor hitches and fps fluctuations. Mario Kart 7 still to test. Vulkan and OpenGL logs for #88/#89 still wanted to confirm the geometry-shader and upload-drop changes.
- Build #91 (v3.0.1 draft): azahar/0044 holds the previous picture on Vulkan while pipelines build (draws skipped on a pending pipeline set g_frame_incomplete; libretro window sends a duplicate frame, max 3 s) and removes the blocking bounded wait; azahar/0050 + KeyboardBridge/OnScreenKeyboard = on-screen keyboard; NES: palette, 4:3 sharp, overscan, IPS/BPS/UPS patches, 60 Hz pacing; gl_fast_mode sets MESA_NO_ERROR=true before the first GL context (auto-off after a GPU crash).
- Still open: R32_UINT storage views of RGBA8 images are null on Xbox (shadows).

## v2.1.0 (Oct 5, 2026 checkpoint)
- Software renderer: banded parallel rasterizer, texture cache, auto frame skip,
  TEV decoded once per triangle, opaque pixels skip the framebuffer read.
  Measured on Series S before the last two changes: NSMB2 menus at full speed with
  frame skip, gameplay at about 20–28 fps; Shakedown Hawaii at 60 fps.
- Hardware renderer: still crashes on Series S as of build #62 (D3D12 device removed
  with DXGI_ERROR_INVALID_CALL during Azahar's renderer start-up). The leading
  suspect, an R32_UINT view of a mutable RGBA8 image that the Xbox can't cast
  (no relaxed format casting without the Agility SDK), now becomes a null
  descriptor (patches/mesa/0011). patches/mesa/0010 logs the exact D3D12 call after
  which the device was removed. Not yet confirmed on hardware.
- UI: bundled Rounded M+ 1c font, Dual Screen (3DS-style) theme, new menu music,
  Restart ONYX after a crash (reopens the game).

## NES support (Oct 6, 2026, not yet built with MSVC or run on a console)
- Second libretro system: FCEUmm built in CI as `fceumm_libretro.dll` (job `fceumm`,
  `tools/build-fceumm.ps1` + `tools/fceumm/CMakeLists.txt`, source pinned in
  `vendor/fceumm/BASE_COMMIT`). The job is allowed to fail: no DLL means the package ships
  without it and the NES tab and folder help say so. Loaded with LoadPackagedLibrary on the
  emulation thread under the crash guard.
- `EmulatorSession` calls cores through `CoreApi` (static Azahar symbols or GetProcAddress).
  NES: software video (XRGB8888 / RGB565 / 0RGB1555 converted in shell/pixel), ROM handed over in
  memory via GET_GAME_INFO_EXT, battery RAM in LocalState/nes/<name>.srm (flushed every ~10 s,
  on pause and on quit; mirrored by SaveBackup), save states under states/nes-<hash>/,
  RetroAchievements/cheats/3DS options skipped.
- Library: `GameLibrary(fs, cache/nes, GameSystem::Nes)` scans `FolderKind::NesRoms`
  (default `Roms/NES`); the 3DS library skips that folder. Home page tabs 3DS | NES (LT / RT).
- Shell tests: 41 cases, 268 assertions.

## NES picture, colours and patches (Oct 6, 2026, not yet built with MSVC or run on a console)
- From the v3.0.0 tester notes (colours too saturated, SMB glitches and hitches, no logs). Causes found in the source: the
  FCEUmm "default" palette (mean saturation 0.67 like the other vivid ones; the FBX composite/NTSC palettes are ~0.62-0.63);
  the NES picture used the global "Smooth" filter (bilinear at 4.5x) at 256:224 (square pixels, not 8:7); pacing at
  60.0988 fps on a 60 Hz screen (a repeated/skipped picture every ~10 s); only 3 upload buffers for a 245 KB frame;
  battery save written on the emulation thread. Not proven on hardware: check the new "Display: N frames dropped" and
  "NES: core reports ..." log lines.
- Defaults: palette `composite-direct-fbx`, 4:3, hide 8 px on every side (core options `fceumm_overscan_*`), Sharp
  filter, NTSC paced at exactly 60 Hz (audio rate nudge covers the 0.16%). `Settings.nes` + `nes_patch`.
- ROM patches: `shell/src/patch.cpp` (IPS, BPS, UPS with CRC32 checks), folder kind `NesPatches`
  (`Patches/NES` on the drive layout), game page "Patches".

## On-screen keyboard for 3DS text requests (Oct 6, 2026, not yet built with MSVC or run on a console)
- Games that open the 3DS software keyboard (names, numbers, messages) get an ONYX keyboard over the game instead
  of the console user name. azahar/0050: `DefaultKeyboard::Execute` calls an app hook and leaves the request open;
  the app answers through `Frontend::SubmitKeyboardText` (core validates, returns the error) or
  `AbortKeyboardRequest`. Without a hook the old behaviour (answer with the user name) stays.
- Flow: hook (emulation thread) -> `Emu/KeyboardBridge` -> `RunOnUi` -> `Ui/OnScreenKeyboard` (built in code, only
  while open) -> answer into a mailbox -> `KeyboardBridge::Pump()` applies it on the emulation thread just before
  `retro_run`. Refused text comes back as a message under the field; the game's own callback refusal reopens it with
  the text kept.
- Controls: D-pad / left stick move, A types, B delete, X space, Y Shift (tap again: Caps), LB/RB cursor, LT/RT key
  page (letters, symbols, accents), Menu = OK, View = Cancel, hold View 1.5 s = close whatever the game wants. USB
  keyboard and mouse work too. The game gets no controller input and the ONYX menu hotkeys are off while it is open.
- Text model and key layouts: `shell/src/text_input.cpp` (unit tested).
- Unverified: everything on a console. Not modelled: custom number-pad keys, predictive input, the game's initial text.

## Planned for the next update (collected 2026-10-07, work starts Monday 10/12 after 4pm)
Playtest results on v3.0.2 (tester): keyboard works well; Tomodachi Life minor hiccups; Mii Maker fine;
Mario Kart 7 37-60 fps; 3D Land 40-60 fps; NSMB2 and Shakedown: Hawaii flawless; NES perfect;
Animal Crossing and Ace Attorney still to report.
1. Netplay: feasibility first.
2. Vulkan performance overhaul (major boost wanted). Starts with the shader (pipeline) cache saved to disk to
   cut first-visit hitching and late graphic load-in. Then the frame hand-off (hardware frames are read back
   and re-uploaded to the presenter; a shared-texture path would remove that copy), Dozen stalls, and a
   profiling pass on Mario Kart 7 and 3D Land to find what else costs frames. Measure before and after.
3. GBA support, reusing the NES plumbing.
4. SteamGridDB: let the user choose which game a cover belongs to instead of guessing
   (Smash 3DS got Smash N64 art; Mario Maker got a DMCA-takedown image). Rank 3DS results first.
5. Encrypted eShop Ace Attorney will not boot with aes_keys.txt in the System folder (tried the app folder
   and the USB). Get the log; show a clear "missing key / seeddb.bin" message instead of a generic failure.
   Check that the System-folder setting actually maps to the folder the user picked.
6. Optional XMB-style (PS3-like) home layout.
7. OpenGL returns later, built from public upstream Mesa sources and our own patches (GlHost.cpp is ours).
Hypothetical, exploration only: GameCube/Wii via Dolphin, Switch (2027).


## Monday 2026-10-12 progress
- Mesa patch 0024: a refused Dozen pipeline now logs its full state (blend, render-target and depth formats, input layout, rasterizer, topology). The refused pipelines pass DXIL validation, so the cause is elsewhere in the state; the next log from Smash, Tomodachi Life or the Ocarina title screen will name it. The refusals explain the missing Tomodachi island and the Smash glitches.
- Audio: the output is un-muted when a game starts and after it quits, so a game quit from the quick menu no longer leaves the next one silent.
- SteamGridDB picker: Get artwork now lists the games SteamGridDB found (3DS-era releases ranked first, so Smash 3DS beats the N64 game) for the user to pick, plus a paste-a-picture-link box (PNG/JPEG, works without an API key). The automatic scraper uses the same ranking. Untested on the console.
- Closing a game sometimes sticks on "Saving and closing" after a few games (reported 10/12, no log): every step of closing is now timed in the log ("Closing: ..."), and vkDeviceWaitIdle is bounded to 8 s so a stuck GPU can no longer hang the close forever. Cause still unconfirmed.
- Still open: texture-view fallback (format 42 from 27, 4-4-4-4 / 5-5-5-1 fallbacks), hold-previous-picture tuning, frame-drop and copy work at the presenter, stream-buffer churn, shader cache, SteamGridDB picker.

### Monday 2026-10-12 — vertex fix + upload measurement
- Pipeline refusals traced to unaligned 4-byte vertex attributes (Dozen refuses the PSO). Azahar 0045 repacks vertex data to aligned offsets on Dozen. Untested on device: Tomodachi island, Smash stages.
- Azahar 0045 also logs vertex upload volume every 5 s. Smash wraps its 64 MiB vertex stream roughly every 0.3 s (~200 MB/s), so the next log shows which draws are heavy before a vertex cache is designed.
- Smash bottom-screen noise and the Zelda logo spots are still open. The only driver warning in those logs is the R32_UINT view of an RGBA8 resource (null descriptor); not proven to be the cause.

### Next build scope (set Mon 2026-10-12)
- Full fixes for the graphical errors (Tomodachi island, Smash stages and bottom-screen noise, Zelda logo spots, Mario Kart hitching).
- Performance: vertex upload volume, presenter drops, extra frame copy, shader cache with background pipeline builds.
- GBA support, polished from the first run: picture settings right off the bat, ROM hacks supported.
- XMB-style menu (original assets only).
- Quick menu: smoother opening, better looking.
- The rest of the planned extras.

### Tuesday 2026-10-12 late — 4.0.0 soft-launch build (CI green, untested on device)
- Tomodachi island confirmed fixed by the vertex alignment patch (azahar 0045); assets still appear ~3 s after the scene.
- Azahar 0046: vertex blocks already uploaded in the same submission are reused (Zelda title re-uploaded ~1.4 MB per draw, ~2 GB/s). Log line "Vertex uploads" shows hit rates.
- GBA: mGBA libretro core (vendor/mgba, patches/mgba make it UWP-safe, GB core on for libretro). Library tab, Roms/GBA and Patches/GBA folders, IPS/BPS/UPS patching in memory, SRAM saves, picture settings (colours, shape, upscaling look, ghosting blend, softer sound) in quick menu and Settings.
- Quick menu: animated open and close, cover art header with clock, focus zoom on buttons, side panels slide in, loading overlay fades.
- Crossbar (XMB-style) home layout: Settings > Interface > "Crossbar home screen". Recent, Favourites and per-system categories; D-pad/triggers/bumpers change category.
- Smash: its glitches were most likely the refused pipelines (same cause as Tomodachi); the vertex alignment fix applies. Zelda logo spots: no cause found in the logs (format fallbacks to RGBA8 are standard conversion paths); needs a new log/photo.
- Not started: disk shader cache, presenter drop work, extra frame copy.
- Zelda leap (untested): the log shows async pipeline builds of 1-3 s on Dozen; every draw skipped meanwhile leaves a hole that stays in cached render targets (logo spots). New setting "Complete graphics" (default on, Settings > Emulation, azahar 0047) waits for each pipeline instead. Costs short pauses the first time an effect appears; turn off to get the old behaviour.

### Tuesday 2026-10-13 early — SteamGridDB speed, crossbar hover panel, pipeline cache (CI green, untested on device)
- SteamGridDB slow/missing covers: the auto-fetch ran one game at a time, retried every unmatched game on every launch (worse with GBA/NES ROM hacks), could hang on a stalled download and wanted exact-size grids only. Now: 4 games in parallel, cover first then hero/logo with each tile updating as it lands, misses remembered for 7 days (cache/art/misses.txt), any-size grid fallback, 30 s body timeout, max 3 download tries per picture.
- Crossbar hover panel (Settings > Interface > "Game details on the crossbar"): SteamGridDB hero as banner with logo, play facts, then after 350 ms description, release date, developer, publisher, genre (Wikipedia, no key) and two screenshots (libretro thumbnail project, community-made). Cached in cache/info. No clips. The two web sources could not be reached from the build machine, so the live format of their replies is untested; parsers are unit-tested on sample data.
- Mesa 0025: compiled pipeline states are kept in an ID3D12PipelineLibrary per game (cache/pso/<game>.bin, setting "Remember compiled graphics"). Log lines "PSO cache loaded / saved / closed: N loaded from cache, N compiled". Falls back silently to compiling if the console refuses libraries.
- Not done from the performance list: frame hand-off copy (readback to the presenter), presenter drop tuning.

### Next build scope (set Mon 2026-10-12 night): performance only
- Focus: Vulkan on Old 3DS mode (60 and 30 fps options), then stabilise Vulkan for New 3DS mode. No new features.
- Tester note: "Complete graphics" did not fix the Zelda logo spots, but models looked better. Logo is low priority.
- Waiting on the test of the pipeline cache and SteamGridDB (logs wanted: "PSO cache" lines, "Artwork:" lines).
- Cartridge tile style: scrapped by the tester (Mon 2026-10-12 night). Crossbar stays an optional setting (already is, off by default).
- Added to the next build (tester, Mon 2026-10-12 night): crossbar hover panel gets a screenshot slideshow (slow pan and crossfade) behind the panel using the screenshots already fetched. Must cost almost nothing; performance work stays the priority.
- Logs wanted for the performance work: Old 3DS mode at 60 and at 30 on Mario Kart 7, Super Mario 3D Land and Zelda (each twice, to see the pipeline cache); the same in New 3DS mode; one long session closed normally; one log with the home screen sitting open for a minute after a fresh start (artwork timing).

## Performance pass 1 (untested, CI only)
- Pipeline cache env var now set with SetEnvironmentVariableW and read with GetEnvironmentVariableA in Dozen (the CRT copy was invisible to the DLL). Expect "PSO cache loaded/saved" lines; second run of a game should show far fewer "pipeline variant compiled" lines. If it is off the log now says why.
- Presenter upload buffers 6 -> 8, and large (scaled) frames now use 4 instead of 3, to cut the ~2.8% dropped frames seen in the MK7 log.
- Hover panel: screenshots now play as a slow crossfading, slowly zooming slideshow (composition animations only).
- Steady state in the MK7 log is 5 ms/frame; the hitches are pipeline compiles (450-500 ms each) and first-visit shader waits, which the cache is meant to remove.
- Unresolved: null descriptor for the shadow storage view (R32_UINT of an RGBA8 image) - shadows from Azahar's shadow path cannot render on Dozen without relaxed format casting; not confirmed as the cause of the MK7 track gap or Zelda model artifacts.
- Shadow rendering on Dozen (patch 0051, untested): the shadow write/read path now uses a companion R32_UINT image per surface (copied from the real RGBA8 image before a shadow pass and back after it; refreshed before shadow reads), because D3D12 here cannot view RGBA8 as R32_UINT. Expect no more "can't be cast" warnings; if the device is removed or shadows look wrong, this is the first suspect. Only active on Dozen.
- Zelda (OoT3D): user found "Hardware shaders" off fixes the model glitches (and runs 10-13 ms/frame at the 30 fps lock). Now a built-in default for the JP/US/EU title IDs (player's own per-game setting still wins). Cause on Dozen with the GPU vertex-shader path is unknown; not other games.
- PSO cache: log from build 125 showed the Dozen library latched "off" when the home-screen device was created (variable not yet set) and never retried. Mesa patch 0027 re-checks per pipeline. Real compile times in that log: 0.5-1.7 s each, 55+ in the first 20 s of the menu - the cache should matter a lot.
- Zelda hardware-shader hunt (patch 0052, hypothesis, untested): on Dozen 3-component byte/short vertex attributes are read as 4-component and the shader forced w=0; the PICA (and drivers with native formats) give w=1, so positions/weights lost their translation. Now w=1. To verify: turn "Hardware shaders" back ON for OoT3D in build 126+; if Link looks right, remove the built-in Zelda override in shell/src/settings.cpp.
- BUILD 127 CRASH (Zelda, hardware shaders on): log shows the D3D12 device was already removed (0x887A0020, driver internal error) when "PSO cache unavailable (0x887a0005)" was logged, i.e. at the first ID3D12Device1::CreatePipelineLibrary of the game - which had never run in-game before (the env var was invisible until 127). Most likely cause: the library creation itself, not the shader change. The cache is now OFF by default and migrated off (settings v5), labelled experimental; Mesa gives up after one failure. The hitch work needs a different approach (see below) until the library is shown safe.
- Zelda confirmed on device (build 128, hardware shaders on): Link and logo render correctly, shadows visible. Built-in hardware-shader-off override removed. Pipeline cache crash confirmed as the library, cache stays off.

## OpenGL (our own) — started, unverified
- New Mesa patches 0029/0030: d3d12 gallium builds for UWP; `onyx_gl` target = our own window-less front end (no WGL/GDI/user32) exporting create/make-current/destroy/get-proc-address/last-error.
- `tools/build-mesa-gl.ps1` + optional CI job `mesagl` (cannot break the package; the DLL is import-checked against WindowsApp.lib before it is allowed into the package).
- `GlHost` now loads `onyx_gl.dll` (core 4.3 context, 1x1 stub default framebuffer, core draws into its own FBO, PBO read-back as before).
- Settings: "Hardware (OpenGL, experimental)" is selectable again (settings v6). Falls back to Vulkan when the DLL is not in the package.
- Nothing here has been built yet; first CI run will show the Mesa compile/link problems, if any.
- CI result: onyx_gl.dll builds, links, passes the Xbox import check and is in the package (build with "OpenGL: driver messages go to the app log"). NOT yet run on a console. Fixes it took: no WindowFromDC/GetModuleFileName on UWP, GL headers before windows.h, Mesa's C11 thread functions renamed (the app runtime exports the same names), zlib off, configure with desktop libs and keep the UWP link line on the target only.

## Persistent vertex cache (Azahar patch 0054, unverified on console)
- Vertex blocks are content-addressed (XXH3 of the guest bytes + layout) and kept in 4 x 24 MiB arenas; a repeat draw binds the old copy instead of copying. Arenas are recycled oldest first after waiting for the GPU.
- Switches itself off (no hashing) while under 20% of the bytes are found again, probing once per few seconds.
- Bookkeeping is tested on Linux (stale-data, recycle-wait and bypass tests); the Vulkan side is only compile-tested by CI. The 5 s "Vertex data:" log line now shows MiB wanted / served by the cache / uploaded.

## Vertex cache v2 (Azahar 0055/0056, unverified on console)
- Replaces the 0054 hash cache, which made MK7 slower (XXH3 over ~10 GB/s was the top CPU cost) and could recycle an arena an earlier loader of the same draw pointed into (corrupted characters).
- Keyed by loader base address + layout; one block per buffer covering every drawn range; invalidated through the rasterizer cache's page tracking (CPU writes, DMA, accelerated GPU transfers). Volatile buffers (rewritten within 2 frames of a copy, 3 times) use per-draw uploads for 10 s; heavily read buffers are untracked.
- Vulkan: 6 x 32 MiB arenas, recycle waits on the last tick that read the arena; arenas used by the current draw are pinned. OpenGL: 6 x 32 MiB GL buffers via glBufferSubData.
- Tested: 10 bookkeeping tests (stale data, recycle waits, pinning, volatility, tracking balance) under ASan. Both rasterizers syntax/warning-checked locally with clang; MSVC build in CI.

## OpenGL crash at first make-current (Mesa 0033)
- onyx_gl.dll+0x2de996 = d3d12_is_format_supported dereferencing the absent sw winsys for PIPE_BIND_DISPLAY_TARGET (decoded with the onyx_gl map now published to the dev release). Fixed; get_fd guarded too.
- GlHost asks for core 3.3 and logs the real version; below 4.3 it lists the missing 4.x extensions.

## Clear new images on Dozen (Azahar 0057, hypothesis, unverified on console)
- Smash log onyx_9 (build 3.1.0.150): no driver warnings, vertex cache mostly bypassed (volatile buffers), yet the photo shows black holes and coloured static where the stage should be. Looks like leftover GPU memory: Dozen places new images in heap memory used a moment earlier, D3D12 zeroes only at heap creation, and Azahar treats undrawn parts of a new surface as empty.
- Every new non-compressed surface image is now cleared once after its first layout change (Dozen only). Costs one barrier pair and a clear per new surface.
- Same hypothesis may also explain the MK7 black characters/white fog and the earlier Zelda logo spots; none of this is confirmed. If Smash still shows static after this, the cause is elsewhere (texture decode, or the shadow companion copy from 0051, which is the other open MK7 suspect).
- MK7 black characters/fog (onyx_8): not the vertex cache (0 errors, 99% hits, same bytes as the old path) and not skipped draws (complete-graphics mode waits). Still open.

## OpenGL 4.3 on Xbox (Mesa 0034, unverified on console)
- onyx_10 log: the "OpenGL" run was really Vulkan ("OpenGL renderer requested but unavailable; using Vulkan"): the context is 4.1 because Mesa hides ARB_shader_image_load_store (Xbox lacks typed UAV loads for some extra formats), ARB_compute_shader (needs images) and ARB_texture_view (needs relaxed format casting).
- 0034 (MESA_UWP builds only): shader images on, still refused per format without typed UAV load/store; texture views on (Azahar only makes same-format views). Expected: "OpenGL context ... 4.3" and the game really running on OpenGL. Risk: a cast view or an image format the driver can't do fails at use time instead of being hidden.
- Same log: Smash in-game was clean on Vulkan this time while onyx_9 (same renderer, same build) showed static. Consistent with the leftover-memory cause behind 0057.

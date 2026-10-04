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

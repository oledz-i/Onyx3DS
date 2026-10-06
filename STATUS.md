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
- Build #79: OpenGL hardware renderer. Mesa GL-on-D3D12 for UWP (SternXD/mesa-uwp
  26.1.3 + three patches, prebuilt archive from danprice142/Azahar-UWP, vendored in
  vendor/mesa-gl with licences) loaded with LoadPackagedLibrary. GlHost makes a
  window-less WGL context on the emulation thread (Mesa then uses a 1x1 offscreen
  framebuffer, never a CoreWindow swap chain), gives the core an RGBA8 FBO, and reads
  frames back through a 3-slot PBO ring one frame behind, flipped by the presenter.
  A guarded startup probe decides availability; OpenGL falls back to Vulkan, then
  Software. Settings v3 moves Vulkan users to OpenGL once. azahar/0039 carries his
  renderer_opengl fixes (vendor detection, no-binary shader cache, single-context
  shader load, opacity skip). Core built with ENABLE_OPENGL=ON.
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

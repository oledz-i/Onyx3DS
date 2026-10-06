# FCEUmm (NES core)

ONYX plays `.nes` / `.unf` games with FCEUmm, the libretro NES core. It is built in CI
(`tools/build-fceumm.ps1`, `tools/fceumm/CMakeLists.txt`) into `fceumm_libretro.dll`,
shipped at the package root next to the Mesa DLLs and loaded with `LoadPackagedLibrary`.
It is a separate DLL on purpose: if it fails to build or load, the 3DS side is unaffected
and the NES tab is simply not offered.

Source: https://github.com/libretro/libretro-fceumm at commit `BASE_COMMIT` (pinned, the
build script refuses any other revision). The tree is unmodified: ONYX hands the core the
ROM bytes through `RETRO_ENVIRONMENT_GET_GAME_INFO_EXT`, so no file-system patch is needed.

Licence: GNU GPL version 2 or later (see `COPYING`, the same text as upstream's `Copying`).
Because the core is distributed as a separate DLL built from the unmodified source above,
the corresponding source is the pinned upstream commit.

# mGBA (GBA core)

ONYX plays `.gba` games (and ROM hacks of them) with mGBA, through its libretro port. It is
built in CI (`tools/build-mgba.ps1`) into `mgba_libretro.dll`, shipped at the package root
next to the Mesa DLLs and loaded with `LoadPackagedLibrary`. It is a separate DLL on purpose:
if it fails to build or load, the 3DS and NES sides are unaffected and the GBA tab is simply
not offered.

Source: https://github.com/libretro/mgba at commit `BASE_COMMIT` (pinned, the build script
refuses any other revision). The tree is unmodified: ONYX hands the core the ROM bytes in
memory (`retro_game_info.data`), so no file-system patch is needed. Patches (IPS / UPS / BPS)
are applied to those bytes by ONYX before the core sees them; the game file is never changed.

Licence: Mozilla Public License 2.0 (see `COPYING`, the same text as upstream's `LICENSE`).
The core is distributed as a separate DLL built from the unmodified source above, so the
corresponding source is the pinned upstream commit.

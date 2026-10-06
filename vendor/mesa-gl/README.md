# OpenGL on D3D12 runtime (hardware renderer, OpenGL)

`mesa-uwp-26.1.3.zip` holds Mesa's OpenGL driver for Direct3D 12 (`opengl32.dll`,
`libgallium_wgl.dll`, and Mesa's zlib `z-1.dll`), built for the UWP app container.
`tools/build-app.ps1` unpacks the three DLLs into the package root, where
`Emu/GlHost.cpp` loads them with `LoadPackagedLibrary`.

Source: SternXD/mesa-uwp, branch uwp-26.1.3, commit
`5c46f0b8b4278d046345526343761524db7611e2`
(https://github.com/SternXD/mesa-uwp), with the three patches in `patches/`
applied in this order: `azahar-output-size.patch`, `azahar-frame-latency.patch`,
`azahar-pso-failure.patch`.

The archive and patches come from danprice142's Azahar-UWP
(https://github.com/danprice142/Azahar-UWP, branch UWP, `externals/uwp`), which
also documents the build. SHA-256 of the archive:
`AA206F0547291254AE986C723E62A68B1CAC2CBDAD08C94DEC593A3640A0BF43`.

Licences: Mesa is MIT-licensed (`LICENSE.rst`, `licenses/`); `z-1.dll` is zlib
1.3.1 (`ZLIB-LICENSE.txt`).

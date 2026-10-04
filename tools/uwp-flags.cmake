# Injected into the Azahar build with CMAKE_PROJECT_INCLUDE (runs after each
# project() call, once the compiler is known, so if(MSVC) works).
# Visual Studio turns on /sdl (Security Development Lifecycle checks) for
# Windows Store projects, which makes old-but-harmless CRT and Winsock calls in
# third-party libraries hard errors. Switch that back off for this build.
if (MSVC)
    # /FIwinapifamily.h: some headers test WINAPI_FAMILY_PARTITION() before
    # including any Windows header; CMake already defines WINAPI_FAMILY.
    add_compile_options(/sdl- /wd4996 /wd4146 /FIwinapifamily.h)
    # Windows 10 target: cryptopp only enables its OS random generator (used
    # by Azahar for console IDs and amiibo) when it can see this.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS
                            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

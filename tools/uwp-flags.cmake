# Injected into the Azahar build with CMAKE_PROJECT_INCLUDE_BEFORE.
# Visual Studio turns on /sdl (Security Development Lifecycle checks) for
# Windows Store projects, which makes old-but-harmless CRT and Winsock calls in
# third-party libraries hard errors. Switch that back off for this build.
if (MSVC)
    # /FIwinapifamily.h: some headers test WINAPI_FAMILY_PARTITION() before
    # including any Windows header; CMake already defines WINAPI_FAMILY.
    add_compile_options(/sdl- /wd4996 /wd4146 /FIwinapifamily.h)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

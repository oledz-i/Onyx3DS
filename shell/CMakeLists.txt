# ONYX 3DS portable shell: library model, settings, cheats, art and
# achievements. Built into the Xbox app and unit-tested on any desktop OS.
cmake_minimum_required(VERSION 3.20)
project(onyx_shell CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ONYX_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# rcheevos (MIT). Compiled as C with hashing enabled so rc_client can
# identify ROMs; the CD reader and RAIntegration are not needed for 3DS.
file(GLOB RCHEEVOS_SOURCES
    ${ONYX_THIRD_PARTY}/rcheevos/src/*.c
    ${ONYX_THIRD_PARTY}/rcheevos/src/rapi/*.c
    ${ONYX_THIRD_PARTY}/rcheevos/src/rcheevos/*.c
    ${ONYX_THIRD_PARTY}/rcheevos/src/rhash/*.c
    ${ONYX_THIRD_PARTY}/rcheevos/src/rurl/*.c)
list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "rc_client_raintegration\\.c$|rc_libretro\\.c$")
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC ${ONYX_THIRD_PARTY}/rcheevos/include)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_DISABLE_LUA)
if(MSVC)
    target_compile_definitions(rcheevos PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(onyx_shell STATIC
    src/common.cpp
    src/n3ds_formats.cpp
    src/paths.cpp
    src/settings.cpp
    src/library.cpp
    src/cheats.cpp
    src/steamgriddb.cpp
    src/achievements.cpp
    src/theme.cpp
    src/core_options.cpp)
target_include_directories(onyx_shell PUBLIC include ${ONYX_THIRD_PARTY})
target_link_libraries(onyx_shell PUBLIC rcheevos)
if(MSVC)
    target_compile_options(onyx_shell PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(onyx_shell PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

option(ONYX_SHELL_TESTS "Build the shell unit tests" ON)
if(ONYX_SHELL_TESTS)
    enable_testing()
    add_executable(onyx_shell_tests
        tests/main.cpp
        tests/test_formats.cpp
        tests/test_settings.cpp
        tests/test_library.cpp
        tests/test_cheats.cpp
        tests/test_services.cpp)
    target_link_libraries(onyx_shell_tests PRIVATE onyx_shell)
    add_test(NAME onyx_shell_tests COMMAND onyx_shell_tests)
endif()

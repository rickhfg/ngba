cmake_minimum_required(VERSION 3.20)

project(ngba VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if (NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -fomit-frame-pointer -DNDEBUG" CACHE STRING "Release flags" FORCE)
endif()

add_library(ngba_core STATIC
    src/ngba/arm7.cpp
    src/ngba/bus.cpp
    src/ngba/ppu.cpp
    src/ngba/rom.cpp
    src/ngba/native.cpp
    src/ngba/runtime.cpp
    src/ngba/file_io.cpp
    src/ngba/rtc.cpp
    src/ngba/apu.cpp
)

target_include_directories(ngba_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(ngba_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ngba_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ngba_probe
    src/main.cpp
)
target_link_libraries(ngba_probe PRIVATE ngba_core)

# The MinGW toolchain is portable on D:, but Explorer does not inherit its
# `bin` directory in PATH. Link the small probe and test binaries against the
# GCC/C++ runtimes statically so they can be launched by double-clicking.
if (MINGW)
    target_link_options(ngba_probe PRIVATE -static-libgcc -static-libstdc++)
endif()

add_executable(ngba_trace
    src/trace.cpp
)
target_link_libraries(ngba_trace PRIVATE ngba_core)
if (MINGW)
    target_link_options(ngba_trace PRIVATE -static-libgcc -static-libstdc++)
endif()

add_executable(ngba_frame
    src/frame.cpp
)
target_link_libraries(ngba_frame PRIVATE ngba_core)
if (MINGW)
    target_link_options(ngba_frame PRIVATE -static-libgcc -static-libstdc++)
endif()

include(CTest)
add_executable(ngba_capture src/capture.cpp)
target_link_libraries(ngba_capture PRIVATE ngba_core)
if(MINGW)
    target_link_options(ngba_capture PRIVATE -static-libgcc -static-libstdc++)
endif()
if (WIN32)
    add_executable(ngba WIN32 src/windows.cpp)
    target_link_libraries(ngba PRIVATE ngba_core user32 gdi32 comdlg32 shell32 winmm ole32)
    if (MINGW)
        target_link_options(ngba PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

add_executable(profile_bench tests/profile_bench.cpp)
target_link_libraries(profile_bench PRIVATE ngba_core)
if (MINGW)
    target_link_options(profile_bench PRIVATE -static-libgcc -static-libstdc++)
endif()


if (BUILD_TESTING)
    if (WIN32)
        add_executable(ngba_windows_tests tests/windows_tests.cpp)
        target_link_libraries(ngba_windows_tests PRIVATE ngba_core user32 gdi32 comdlg32 shell32 winmm ole32)
        if (MINGW)
            target_link_options(ngba_windows_tests PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        if (MSVC)
            target_compile_options(ngba_windows_tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(ngba_windows_tests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ngba_windows_tests COMMAND ngba_windows_tests)
        set_tests_properties(ngba_windows_tests PROPERTIES TIMEOUT 30)
    endif()
    add_executable(ngba_file_io_tests tests/file_io_tests.cpp)
    target_link_libraries(ngba_file_io_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_file_io_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_file_io_tests COMMAND ngba_file_io_tests)
    add_executable(ngba_bios_tests tests/bios_tests.cpp)
    target_link_libraries(ngba_bios_tests PRIVATE ngba_core)
    if(MINGW)
        target_link_options(ngba_bios_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_bios_tests COMMAND ngba_bios_tests)
    add_executable(ngba_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(ngba_runtime_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_runtime_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_runtime_tests COMMAND ngba_runtime_tests)
    add_executable(ngba_rom_tests
        tests/rom_tests.cpp
    )
    target_link_libraries(ngba_rom_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_rom_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_rom_tests COMMAND ngba_rom_tests)

    add_executable(ngba_arm7_tests
        tests/arm7_tests.cpp
    )
    target_link_libraries(ngba_arm7_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_arm7_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_arm7_tests COMMAND ngba_arm7_tests)

    add_executable(ngba_ppu_tests
        tests/ppu_tests.cpp
    )
    target_link_libraries(ngba_ppu_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_ppu_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_ppu_tests COMMAND ngba_ppu_tests)
    add_executable(ngba_rtc_tests
        tests/rtc_tests.cpp
    )
    target_link_libraries(ngba_rtc_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_rtc_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_rtc_tests COMMAND ngba_rtc_tests)

    add_executable(ngba_apu_tests
        tests/apu_tests.cpp
    )
    target_link_libraries(ngba_apu_tests PRIVATE ngba_core)
    if (MINGW)
        target_link_options(ngba_apu_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME ngba_apu_tests COMMAND ngba_apu_tests)

    foreach(test_target ngba_rom_tests ngba_arm7_tests ngba_ppu_tests ngba_runtime_tests ngba_bios_tests ngba_file_io_tests ngba_rtc_tests ngba_apu_tests)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
endif()

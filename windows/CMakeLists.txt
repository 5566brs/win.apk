# Win.apk -- Windows x64 port. Phase 1: the Android x86_64 ELF loader PoC.
#
# Needs clang targeting Windows x64:
#   * on Windows: MSYS2 CLANG64 (what CI uses)
#   * on Linux:   cross-compile with -DCMAKE_TOOLCHAIN_FILE=windows/cmake/clang-mingw-x86_64.cmake
# MSVC cannot emit System V calls at all, and mingw GCC 13 emits broken SEH
# directives for sysv_abi functions that unwind (".seh_handlerdata used outside
# of .seh_proc block"), so both are rejected.
#
#   cmake -S windows -B windows/build -G Ninja && cmake --build windows/build

cmake_minimum_required(VERSION 3.20)
project(winapk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Win.apk needs clang (found ${CMAKE_CXX_COMPILER_ID}); see the note above.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Win.apk targets 64-bit Windows only.")
endif()

add_library(winapk_loader STATIC
  src/elf_loader.cpp
  src/fs_tls_patch.cpp
  src/mini_bionic.cpp
)
target_include_directories(winapk_loader PUBLIC include)
target_compile_definitions(winapk_loader PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT MSVC)
  target_compile_options(winapk_loader PRIVATE -Wall -Wextra)
endif()

add_executable(winapk-elfrun tools/elfrun_main.cpp)
target_link_libraries(winapk-elfrun PRIVATE winapk_loader)
if(MINGW)
  # One self-contained .exe: no libstdc++/libgcc/winpthread DLLs next to it.
  target_link_options(winapk-elfrun PRIVATE -static)
endif()

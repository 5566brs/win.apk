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
  src/abi_thunk.cpp
  src/linker.cpp
)
target_include_directories(winapk_loader PUBLIC include)
target_compile_definitions(winapk_loader PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT MSVC)
  target_compile_options(winapk_loader PRIVATE -Wall -Wextra)
endif()

add_executable(winapk-elfrun tools/elfrun_main.cpp)
target_link_libraries(winapk-elfrun PRIVATE winapk_loader)

add_executable(winapk-thunk-test test/thunk_test.cpp)
target_link_libraries(winapk-thunk-test PRIVATE winapk_loader)

add_executable(winapk-linker-test test/linker_test.cpp)
target_link_libraries(winapk-linker-test PRIVATE winapk_loader)

if(MINGW)
  # One self-contained .exe: no libstdc++/libgcc/winpthread DLLs next to it.
  target_link_options(winapk-elfrun PRIVATE -static)
  target_link_options(winapk-thunk-test PRIVATE -static)
  target_link_options(winapk-linker-test PRIVATE -static)
endif()

# The JNI bridge needs jni.h + win32/jni_md.h. Point WINAPK_JNI_INCLUDE at a
# JDK's include directory (e.g. "$JAVA_HOME/include"); the loader above stays
# JDK-free, so the elf-loader and thunk targets build without a JDK.
set(WINAPK_JNI_INCLUDE "" CACHE PATH "JDK include dir (contains jni.h and win32/jni_md.h)")
if(WINAPK_JNI_INCLUDE)
  add_library(winapk_jni STATIC src/guest_jni.cpp src/jni_variadic.cpp)
  target_include_directories(winapk_jni PUBLIC include "${WINAPK_JNI_INCLUDE}" "${WINAPK_JNI_INCLUDE}/win32")
  target_link_libraries(winapk_jni PUBLIC winapk_loader)

  add_executable(winapk-jni-test test/jni_wrapper_test.cpp)
  target_link_libraries(winapk-jni-test PRIVATE winapk_jni)

  add_executable(winapk-jni-variadic-test test/jni_variadic_test.cpp)
  target_link_libraries(winapk-jni-variadic-test PRIVATE winapk_jni)

  # The real-JVM end-to-end harness. It loads jvm.dll at runtime (not linked),
  # so it only needs the JNI/JVMTI headers.
  add_executable(winapk-jvm-run tools/jvmrun_main.cpp)
  target_link_libraries(winapk-jvm-run PRIVATE winapk_jni)

  if(MINGW)
    target_link_options(winapk-jni-test PRIVATE -static)
    target_link_options(winapk-jni-variadic-test PRIVATE -static)
    target_link_options(winapk-jvm-run PRIVATE -static)
  endif()
else()
  message(STATUS "WINAPK_JNI_INCLUDE not set; skipping the JNI wrapper target (winapk_jni, winapk-jni-test)")
endif()

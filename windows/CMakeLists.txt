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
  add_library(winapk_jni STATIC src/guest_jni.cpp src/jni_variadic.cpp src/jni_bind.cpp)
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

  # SQLite for NativeSqlite: the official amalgamation (public domain),
  # downloaded once at configure time and checked against the SHA3-256 hash
  # sqlite.org publishes.
  # The feature set follows what Android's SQLite enables for apps.
  set(WINAPK_SQLITE_VERSION 3540100)
  set(WINAPK_SQLITE_SHA3_256 92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30)
  set(WINAPK_SQLITE_DIR ${CMAKE_BINARY_DIR}/sqlite-amalgamation-${WINAPK_SQLITE_VERSION})
  if(NOT EXISTS ${WINAPK_SQLITE_DIR}/sqlite3.c)
    set(_zip ${CMAKE_BINARY_DIR}/sqlite-amalgamation-${WINAPK_SQLITE_VERSION}.zip)
    file(DOWNLOAD https://www.sqlite.org/2026/sqlite-amalgamation-${WINAPK_SQLITE_VERSION}.zip ${_zip}
         EXPECTED_HASH SHA3_256=${WINAPK_SQLITE_SHA3_256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT ${_zip} DESTINATION ${CMAKE_BINARY_DIR})
  endif()
  enable_language(C)
  add_library(winapk_sqlite STATIC ${WINAPK_SQLITE_DIR}/sqlite3.c)
  target_include_directories(winapk_sqlite PUBLIC ${WINAPK_SQLITE_DIR})
  target_compile_definitions(winapk_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_PARENTHESIS SQLITE_ENABLE_FTS4
    SQLITE_ENABLE_FTS5 SQLITE_ENABLE_RTREE SQLITE_ENABLE_COLUMN_METADATA)

  # The host that runs an APK (milestone M1). Like winapk-jvm-run it loads
  # jvm.dll at runtime.
  add_executable(winapk-runtime host/runtime_main.cpp host/host_natives.cpp host/native_log.cpp
                              host/native_cocoa.cpp host/win32_window.cpp)
  target_link_libraries(winapk-runtime PRIVATE winapk_jni winapk_sqlite windowscodecs ole32)
  target_compile_options(winapk-runtime PRIVATE -Wall -Wextra)
  # Upstream's NativeSqlite bridge, compiled unchanged: compat/ supplies the
  # few CoreFoundation string calls it makes. Upstream's warnings are not ours.
  target_sources(winapk-runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_sqlite.cpp)
  target_include_directories(winapk-runtime PRIVATE compat)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_sqlite.cpp
                              PROPERTIES COMPILE_OPTIONS "-w")

  if(MINGW)
    target_link_options(winapk-runtime PRIVATE -static -municode)
    target_link_options(winapk-jni-test PRIVATE -static)
    target_link_options(winapk-jni-variadic-test PRIVATE -static)
    target_link_options(winapk-jvm-run PRIVATE -static)
  endif()
else()
  message(STATUS "WINAPK_JNI_INCLUDE not set; skipping the JNI wrapper target (winapk_jni, winapk-jni-test)")
endif()

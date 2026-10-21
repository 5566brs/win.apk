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
  src/elf_tls.cpp
  src/fs_tls_patch.cpp
  src/mini_bionic.cpp
  src/bionic/string.cpp
  src/bionic/printf.cpp
  src/bionic/pthread.cpp
  src/bionic/stdio.cpp
  src/bionic/misc.cpp
  src/bionic/locale.cpp
  src/bionic/paths.cpp
  src/bionic/fd.cpp
  src/bionic/socket.cpp
  src/bionic/looper.cpp
  src/abi_thunk.cpp
  src/linker.cpp
)
target_include_directories(winapk_loader PUBLIC include PRIVATE src)
# WaitOnAddress (the shim's futexes), BCryptGenRandom (arc4random), Winsock.
target_link_libraries(winapk_loader PUBLIC synchronization bcrypt ws2_32)
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

add_executable(winapk-bionic-test test/bionic_test.cpp)
target_link_libraries(winapk-bionic-test PRIVATE winapk_loader)

if(MINGW)
  # One self-contained .exe: no libstdc++/libgcc/winpthread DLLs next to it.
  target_link_options(winapk-elfrun PRIVATE -static)
  target_link_options(winapk-thunk-test PRIVATE -static)
  target_link_options(winapk-linker-test PRIVATE -static)
  target_link_options(winapk-bionic-test PRIVATE -static)
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

  # libwebp's decoder for NativeImageCodec: most APK drawables are WebP, which
  # Java's ImageIO cannot read and not every Windows has a WIC codec for. The
  # release tarball, checked against its SHA-256; only the decoder is built.
  set(WINAPK_WEBP_VERSION 1.5.0)
  set(WINAPK_WEBP_SHA256 7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c)
  set(WINAPK_WEBP_DIR ${CMAKE_BINARY_DIR}/libwebp-${WINAPK_WEBP_VERSION})
  if(NOT EXISTS ${WINAPK_WEBP_DIR}/CMakeLists.txt)
    set(_tgz ${CMAKE_BINARY_DIR}/libwebp-${WINAPK_WEBP_VERSION}.tar.gz)
    file(DOWNLOAD https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WINAPK_WEBP_VERSION}.tar.gz
         ${_tgz} EXPECTED_HASH SHA256=${WINAPK_WEBP_SHA256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT ${_tgz} DESTINATION ${CMAKE_BINARY_DIR})
  endif()
  foreach(_opt ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
    set(WEBP_BUILD_${_opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  add_subdirectory(${WINAPK_WEBP_DIR} ${CMAKE_BINARY_DIR}/libwebp-build EXCLUDE_FROM_ALL)

  # stb_vorbis (public domain) decodes the Ogg Vorbis clips games ship for
  # SoundPool and MediaPlayer; Windows has no Vorbis codec. One file, pinned to
  # a commit and checked against its SHA-256.
  set(WINAPK_STB_COMMIT a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d)
  set(WINAPK_STB_VORBIS ${CMAKE_BINARY_DIR}/stb_vorbis-${WINAPK_STB_COMMIT}.c)
  if(NOT EXISTS ${WINAPK_STB_VORBIS})
    file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/${WINAPK_STB_COMMIT}/stb_vorbis.c
         ${WINAPK_STB_VORBIS}
         EXPECTED_HASH SHA256=4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636 TLS_VERIFY ON)
  endif()
  add_library(winapk_stb_vorbis STATIC ${WINAPK_STB_VORBIS})
  target_compile_options(winapk_stb_vorbis PRIVATE -w)

  # The host that runs an APK (milestone M1). Like winapk-jvm-run it loads
  # jvm.dll at runtime.
  add_executable(winapk-runtime host/runtime_main.cpp host/host_natives.cpp host/native_log.cpp
                              host/native_cocoa.cpp host/win32_window.cpp host/guest_libs.cpp
                              host/native_os.cpp host/native_image.cpp
                              host/native_looper.cpp host/native_audio.cpp host/audio_engine.cpp
                              host/audio_decode.cpp host/native_clipboard.cpp
                              host/native_wallpaper.cpp host/native_picker.cpp
                              host/native_gamepad.cpp host/native_icu.cpp
                              host/native_audio_input.cpp host/native_photos.cpp)
  target_link_libraries(winapk-runtime PRIVATE winapk_jni winapk_sqlite webpdecoder winapk_stb_vorbis windowscodecs
                        ole32 mfplat mfreadwrite mfuuid shlwapi advapi32 shell32 uuid)
  target_include_directories(winapk-runtime PRIVATE ${WINAPK_WEBP_DIR}/src)
  target_compile_options(winapk-runtime PRIVATE -Wall -Wextra)
  # GL (M6): upstream's android.opengl bindings, cut out of its Mac
  # native_methods.cpp at build time and compiled against the Khronos headers;
  # ANGLE is loaded at run time (host/gl/gl_loader.h).
  set(WINAPK_GL_UPSTREAM ${CMAKE_BINARY_DIR}/gl/gl_upstream.inc)
  add_custom_command(
    OUTPUT ${WINAPK_GL_UPSTREAM}
    COMMAND ${CMAKE_COMMAND} -DUPSTREAM=${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_methods.cpp
            -DOUT=${WINAPK_GL_UPSTREAM} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_gl_bridges.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_methods.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_gl_bridges.cmake
    COMMENT "Extracting upstream's GL bindings")
  target_sources(winapk-runtime PRIVATE host/gl/gl_loader.cpp host/gl/gl_stubs.cpp host/gl/gl_bridges.cpp
                 host/gl/gl_surface.cpp
                 ${WINAPK_GL_UPSTREAM})
  target_include_directories(winapk-runtime PRIVATE host third_party/khronos ${CMAKE_BINARY_DIR}/gl
                             ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src)
  target_compile_definitions(winapk-runtime PRIVATE KHRONOS_STATIC GL_GLEXT_PROTOTYPES=0)
  # Upstream's code, upstream's warnings.
  set_source_files_properties(host/gl/gl_bridges.cpp PROPERTIES COMPILE_OPTIONS "-w")
  # Upstream's NativeSqlite bridge, compiled unchanged: compat/ supplies the
  # few CoreFoundation string calls it makes. Upstream's warnings are not ours.
  target_sources(winapk-runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_sqlite.cpp)
  target_include_directories(winapk-runtime PRIVATE compat)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/native_sqlite.cpp
                              PROPERTIES COMPILE_OPTIONS "-w")

  # The runtime's GL on ANGLE (test/gl_test.cpp), on whatever backend the
  # machine has.
  add_executable(winapk-gl-test test/gl_test.cpp host/gl/gl_surface.cpp host/gl/gl_loader.cpp host/gl/gl_stubs.cpp)
  target_include_directories(winapk-gl-test PRIVATE host third_party/khronos)
  target_compile_definitions(winapk-gl-test PRIVATE KHRONOS_STATIC)
  target_link_options(winapk-gl-test PRIVATE -static)

  # NativeAudio's backend on its own (test/audio_test.cpp).
  add_executable(winapk-audio-test test/audio_test.cpp host/audio_engine.cpp host/audio_decode.cpp)
  target_link_libraries(winapk-audio-test PRIVATE winapk_stb_vorbis ole32 mfplat mfreadwrite mfuuid shlwapi)
  target_link_options(winapk-audio-test PRIVATE -static)

  # An app's native libraries (milestone M4, host/guest_libs.h): the stub DLL
  # that stands for each one and the translator that copies it. Built with the
  # runtime, which loads them.
  add_library(winapk-guest-stub SHARED host/guest_stub.cpp)
  set_target_properties(winapk-guest-stub PROPERTIES PREFIX "")
  target_include_directories(winapk-guest-stub PRIVATE "${WINAPK_JNI_INCLUDE}" "${WINAPK_JNI_INCLUDE}/win32")
  add_executable(macapk-translator host/translator_main.cpp)
  add_dependencies(winapk-runtime winapk-guest-stub macapk-translator)

  if(MINGW)
    target_link_options(winapk-guest-stub PRIVATE -static)
    target_link_options(macapk-translator PRIVATE -static -municode)
    target_link_options(winapk-runtime PRIVATE -static -municode)
    target_link_options(winapk-jni-test PRIVATE -static)
    target_link_options(winapk-jni-variadic-test PRIVATE -static)
    target_link_options(winapk-jvm-run PRIVATE -static)
  endif()
else()
  message(STATUS "WINAPK_JNI_INCLUDE not set; skipping the JNI wrapper target (winapk_jni, winapk-jni-test)")
endif()

# Cross-compile Win.apk for Windows x64 from Linux with clang + the mingw-w64
# headers and libraries (Debian/Ubuntu: apt install clang lld mingw-w64
# libz-mingw-w64-dev).
#
#   cmake -S windows -B windows/build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=windows/cmake/clang-mingw-x86_64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(WINAPK_TRIPLE x86_64-w64-windows-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${WINAPK_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${WINAPK_TRIPLE})
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

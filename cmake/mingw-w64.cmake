# Copyright 2026 Ian Karlsson
# Cross-compiling for Windows with mingw-w64: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(QDOM_MINGW_TRIPLE x86_64-w64-mingw32 CACHE STRING "The mingw-w64 compilers' prefix")
set(CMAKE_C_COMPILER ${QDOM_MINGW_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${QDOM_MINGW_TRIPLE}-g++)
set(CMAKE_RC_COMPILER ${QDOM_MINGW_TRIPLE}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${QDOM_MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# the runtime inside the program or plugin: no libgcc, libstdc++ or libwinpthread DLLs beside it
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")

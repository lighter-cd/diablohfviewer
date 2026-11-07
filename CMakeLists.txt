cmake_minimum_required(VERSION 3.15)
project(diablo_hf_converter CXX)

# ---- C++ standard ----------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- StormLib (prebuilt static lib) ----------------------------------------
# Build it once with:  bash build-stormlib.sh
# (CMake -G "MinGW Makefiles" -DSTORM_USE_BUNDLED_LIBRARIES=ON)
set(SL_DIR      "${CMAKE_SOURCE_DIR}/3rdparty/StormLib")
set(SL_LIB      "${SL_DIR}/build/libStormLib.a")
if(NOT EXISTS "${SL_LIB}")
  message(FATAL_ERROR
    "StormLib not built yet. Run:\n"
    "    bash build-stormlib.sh\n"
    "then re-run cmake.")
endif()

# ---- Include dirs ----------------------------------------------------------
include_directories(
  "${SL_DIR}/src"            # StormLib.h
  "${CMAKE_SOURCE_DIR}/3rdparty"   # nlohmann/json.hpp
  "${CMAKE_SOURCE_DIR}/src"
)

# ---- Sources ---------------------------------------------------------------
file(GLOB_RECURSE PROJECT_SRCS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(mcp-server ${PROJECT_SRCS})

# Statically link the GCC/C++ runtime so the executable is self-contained and
# does not accidentally load an ABI-incompatible libstdc++-6.dll from another
# MinGW on PATH (e.g. Git's bundled one) — that mismatch crashes string/vector
# ops at runtime. The goal is a single portable native executable.
target_link_options(mcp-server PRIVATE -static-libgcc -static-libstdc++)

target_link_libraries(mcp-server
  "${SL_LIB}"
  ws2_32
  wininet
)

# Quiet noisy MinGW warnings from StormLib's bundled code is not needed here
# (we only include headers); link warnings are fine.

cmake_minimum_required(VERSION 3.15)
project(onex_auto_tdp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure MinHook as a static library
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static minhook" FORCE)
add_subdirectory(vendor/minhook)

# Add version.dll proxy target (uses pragma comment linker export forwarding in dllmain.cpp)
add_library(version SHARED
    src/dllmain.cpp
)

target_include_directories(version PRIVATE
    vendor/minhook/include
    src
)

target_link_libraries(version PRIVATE
    minhook
    pdh
    dbghelp
)

# Set output name to version.dll (no lib prefix)
set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
)

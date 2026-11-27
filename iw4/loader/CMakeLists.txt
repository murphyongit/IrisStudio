cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(iw4iris_loader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# the C++ runtime inside the DLL, so nothing else needs installing
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ddraw.dll for the game's folder: see loader.cpp
add_library(loader SHARED loader.cpp ddraw.def)
set_target_properties(loader PROPERTIES OUTPUT_NAME "ddraw" PREFIX "")
target_compile_definitions(loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
install(TARGETS loader RUNTIME DESTINATION ".")

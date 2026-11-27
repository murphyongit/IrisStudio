cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(iw5iris_launcher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# the C++ runtime inside the program, so nothing else needs installing
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# IW5iris.exe: see main.cpp
add_executable(launcher main.cpp)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "IW5iris")
target_compile_definitions(launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(launcher PRIVATE shell32 advapi32)
install(TARGETS launcher RUNTIME DESTINATION ".")

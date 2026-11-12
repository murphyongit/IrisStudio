cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
project(s1iris CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# the C++ runtime inside the DLL, so nothing else needs installing
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4)
FetchContent_MakeAvailable(minhook)

# S1iris loads as XInput1_3.dll from the game's folder (the game asks for it there), passing every real
# request to Windows' own: the version.dll door is the debug overlay's.
# Dear ImGui for the panel (the BO1 mod's own copy is a submodule set up for Direct3D 9)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.9)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})

add_library(s1iris SHARED src/theater.cpp src/ui.cpp src/camera.cpp src/proxy_xinput.cpp)
set_target_properties(s1iris PROPERTIES OUTPUT_NAME "XInput1_3" PREFIX "")
target_compile_definitions(s1iris PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(s1iris PRIVATE minhook imgui d3d11 dxgi)
install(TARGETS s1iris RUNTIME DESTINATION ".")

cmake_minimum_required(VERSION 3.20)
project(ProShadeLite CXX)

# Folderul cu sursele ReShade (git clone https://github.com/crosire/reshade + submodulul deps/imgui)
set(RESHADE_DIR "" CACHE PATH "Calea catre repo-ul ReShade")
if(NOT EXISTS "${RESHADE_DIR}/include/reshade.hpp")
  message(FATAL_ERROR "Seteaza -DRESHADE_DIR=<repo reshade>")
endif()

add_library(ProShadeLite SHARED src/main.cpp src/third_party/state_tracking.cpp)
target_compile_features(ProShadeLite PRIVATE cxx_std_20)
target_include_directories(ProShadeLite PRIVATE
  "${RESHADE_DIR}/include" "${RESHADE_DIR}/deps/imgui" src/third_party)
target_compile_definitions(ProShadeLite PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_ext ".addon64")
else()
  set(_ext ".addon32")
endif()
set_target_properties(ProShadeLite PROPERTIES PREFIX "" SUFFIX "${_ext}")

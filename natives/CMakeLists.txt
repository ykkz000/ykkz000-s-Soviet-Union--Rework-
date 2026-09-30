cmake_minimum_required(VERSION 3.21)
project(YKKZ000Dlls LANGUAGES CXX)

# Diagnostic bisection switches (off by default; used only to attribute a crash):
# - Disable the custom behavior path (no implementation supplied at registration; degrades to
#   template behavior)
option(YKKZ000_DISABLE_CUSTOM_BEHAVIOR
  "Do not supply an implementation when registering effects (disables the custom effect path)" OFF)
option(YKKZ000_DISABLE_EFFECT_HANDLER
  "Disable effect handler registration (the built-in handler table hook)" OFF)
option(YKKZ000_DISABLE_HANDLER_GUARD
  "Disable the handler dispatch guard (for crash reproduction/control only)" OFF)
option(YKKZ000_ENABLE_DISPATCH_TRACE
  "Enable the 0x979290 generic dispatch trace hook (diagnostic, off by default)" OFF)
option(YKKZ000_DISABLE_STRENGTH_PER_SUZERAIN
  "Degrade the per-suzerain combat strength effect to template behavior (register the type only, no scaling)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 builds only")
endif()

if(MSVC)
  # Static CRT: eliminates the VCRUNTIME140_1.dll dependency, preventing the proxy DLL from
  # blocking game startup when a newer VC++ runtime is missing.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
endif()

# Convenience for collecting artifacts uniformly (not deployment)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# Shared headers: INTERFACE target, propagates the include directory through linking
add_library(ykkz000_bridge INTERFACE)
target_include_directories(ykkz000_bridge INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(ykkz000_bridge INTERFACE cxx_std_20)

add_subdirectory(third_party/minhook)
add_subdirectory(loader)
add_subdirectory(plugin-soviet-union)

cmake_minimum_required(VERSION 3.21)
project(YKKZ000Dlls LANGUAGES CXX)

# Diagnostic bisection switches (off by default; used only to attribute a crash):
# - Disable the custom behavior path (no implementation supplied at registration; degrades to
#   template behavior)
option(DISABLE_CUSTOM_BEHAVIOR
  "Do not supply an implementation when registering effects (disables the custom effect path)" OFF)
option(DISABLE_EFFECT_HANDLER
  "Disable effect handler registration (the built-in handler table hook)" OFF)
option(DISABLE_HANDLER_GUARD
  "Disable the handler dispatch guard (for crash reproduction/control only)" OFF)
option(ENABLE_DISPATCH_TRACE
  "Enable the 0x979290 generic dispatch trace hook (diagnostic, off by default)" OFF)
option(DISABLE_STRENGTH_PER_SUZERAIN
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

# Third-party libraries are managed by vcpkg (manifest mode); the vcpkg toolchain resolves the
# imported targets. See natives/vcpkg.json.
find_package(minhook CONFIG REQUIRED)
# log4cxx's exported config references EXPAT::EXPAT, while the vcpkg expat port exports the target
# as expat::expat. Provide the expected name before resolving log4cxx.
find_package(expat CONFIG REQUIRED)
if(NOT TARGET EXPAT::EXPAT)
  add_library(EXPAT::EXPAT INTERFACE IMPORTED)
  set_target_properties(EXPAT::EXPAT PROPERTIES INTERFACE_LINK_LIBRARIES expat::expat)
endif()
find_package(log4cxx CONFIG REQUIRED)

# Shared headers: INTERFACE target, propagates the include directory through linking
add_library(ykkz000_bridge INTERFACE)
target_include_directories(ykkz000_bridge INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(ykkz000_bridge INTERFACE cxx_std_20)

# Logging gate: DEBUG/TRACE calls are compiled in only under the standard _DEBUG macro, which the
# Debug configuration provides (MSVC /MTd). Release/RelWithDebInfo carry zero logging overhead. No
# compile definition is propagated: _DEBUG is toolchain-provided.

add_subdirectory(loader)

# Shared plugin APIs. Each is a real plugin (it exports GetPlugin/GetPluginManifest) so the loader
# initializes it before the consumers that declare it as a manifest dependency.
add_subdirectory(plugin-api-effecttype)
add_subdirectory(plugin-api-persistence)

# Header/link aggregation for consumers: linking ykkz000_plugin_api pulls in both API plugins'
# headers and import libraries.
add_library(ykkz000_plugin_api INTERFACE)
target_link_libraries(ykkz000_plugin_api INTERFACE
  YKKZ000PluginApiEffectType
  YKKZ000PluginApiPersistence)

add_subdirectory(plugin-soviet-union)

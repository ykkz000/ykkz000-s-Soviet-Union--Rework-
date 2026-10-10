cmake_minimum_required(VERSION 3.21)
project(YKKZ000SovietUnion LANGUAGES CXX)

# Diagnostic bisection switches (off by default; used only to attribute a crash):
# - Disable the custom behavior path (no implementation supplied at registration; degrades to
#   template behavior)
option(DISABLE_CUSTOM_BEHAVIOR
  "Do not supply an implementation when registering effects (disables the custom effect path)" OFF)
option(DISABLE_STRENGTH_PER_SUZERAIN
  "Degrade the per-suzerain combat strength effect to template behavior (register the type only, no scaling)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 builds only")
endif()

if(MSVC)
  # Static CRT: eliminates the VCRUNTIME140_1.dll dependency.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
endif()

# Convenience for collecting artifacts uniformly (not deployment)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# Shared headers: INTERFACE target, propagates the include directory through linking. This repo
# links no third-party libraries (see natives/vcpkg.json); the API plugins it consumes are shipped by
# the ykkz000's DLL Plugin API mod and resolved at load time by the loader.
add_library(ykkz000_bridge INTERFACE)
target_include_directories(ykkz000_bridge INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(ykkz000_bridge INTERFACE cxx_std_20)

# Logging gate: DEBUG/TRACE calls are compiled in only under the standard _DEBUG macro, which the
# Debug configuration provides (MSVC /MTd). Release/RelWithDebInfo carry zero logging overhead.

# --- Consumed API import libraries ---
# The consumer links GetEffectTypeApi/GetPersistenceApi, which are exported by the API plugins in the
# ykkz000's DLL Plugin API mod. That mod is not built here; this repo vendors the API .def files and
# generates the matching import libraries with the MSVC librarian (CMAKE_AR, i.e. lib.exe) so the
# imports resolve at link time. The DLLs themselves are resolved at load time (the loader puts every
# enabled plugin directory on the search path).
set(YKKZ000_PLUGIN_API_DEF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/plugin_api")
set(YKKZ000_PLUGIN_API_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/vendor/plugin_api")
file(MAKE_DIRECTORY "${YKKZ000_PLUGIN_API_LIB_DIR}")

set(YKKZ000_PLUGIN_API_IMPORT_LIBS)
foreach(api effecttype persistence)
  set(def "${YKKZ000_PLUGIN_API_DEF_DIR}/ykkz000_plugin_api_${api}.def")
  if(NOT EXISTS "${def}")
    message(FATAL_ERROR "Missing vendored API definition file: ${def}")
  endif()
  set(import_lib "${YKKZ000_PLUGIN_API_LIB_DIR}/ykkz000_plugin_api_${api}.lib")
  add_custom_command(
    OUTPUT "${import_lib}"
    COMMAND "${CMAKE_AR}" /nologo /def:${def} /machine:x64 /out:${import_lib}
    DEPENDS "${def}"
    COMMENT "Generating API import library ykkz000_plugin_api_${api}.lib"
    VERBATIM)
  add_library(ykkz000_plugin_api_${api} STATIC IMPORTED GLOBAL)
  set_target_properties(ykkz000_plugin_api_${api} PROPERTIES IMPORTED_LOCATION "${import_lib}")
  list(APPEND YKKZ000_PLUGIN_API_IMPORT_LIBS "${import_lib}")
endforeach()

add_custom_target(ykkz000_plugin_api_implibs DEPENDS ${YKKZ000_PLUGIN_API_IMPORT_LIBS})

# Aggregation for consumers: linking ykkz000_plugin_api pulls in the shared API header directory (the
# vendored copies under natives/include/ykkz000/plugin) and both API import libraries.
add_library(ykkz000_plugin_api INTERFACE)
target_include_directories(ykkz000_plugin_api INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(ykkz000_plugin_api INTERFACE
  ykkz000_plugin_api_effecttype
  ykkz000_plugin_api_persistence)
add_dependencies(ykkz000_plugin_api ykkz000_plugin_api_implibs)

add_subdirectory(plugin-soviet-union)

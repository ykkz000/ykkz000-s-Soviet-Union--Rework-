project(YKKZ000SovietUnionPlugin LANGUAGES CXX)

add_library(YKKZ000SovietUnionPlugin SHARED
  src/plugin.cpp
  src/gameplay/city_yield_common.cpp
  src/gameplay/object_lifecycle.cpp
  src/effects/adjust_city_yield_modifier_per_population.cpp
  src/effects/adjust_city_yield_modifier_per_suzerain.cpp
  src/effects/adjust_player_strength_modifier_per_suzerain.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000SovietUnionPlugin PROPERTIES
  OUTPUT_NAME "Plugin_YKKZ000_Soviet_Union"
  PREFIX "")

target_include_directories(YKKZ000SovietUnionPlugin PRIVATE include)

# ykkz000_plugin_api aggregates the shared API headers and the import libraries generated from the
# vendored API .def files (see natives/CMakeLists.txt), so the consumer's imports of
# GetEffectTypeApi/GetPersistenceApi resolve at link time and the OS resolves the DLLs at load time
# (the loader puts the plugin directory on the search path).
target_link_libraries(YKKZ000SovietUnionPlugin PRIVATE ykkz000_bridge ykkz000_plugin_api)

# Ensure the import libraries are generated before the consumer is linked.
add_dependencies(YKKZ000SovietUnionPlugin ykkz000_plugin_api_implibs)

if(DISABLE_CUSTOM_BEHAVIOR)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_CUSTOM_BEHAVIOR)
endif()

if(DISABLE_STRENGTH_PER_SUZERAIN)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_STRENGTH_PER_SUZERAIN)
endif()

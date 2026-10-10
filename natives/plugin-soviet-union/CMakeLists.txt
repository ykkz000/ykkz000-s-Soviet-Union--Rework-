project(YKKZ000SovietUnionPlugin LANGUAGES CXX)

add_library(YKKZ000SovietUnionPlugin SHARED
  src/plugin.cpp
  src/core/engine_access.cpp
  src/gameplay/city_yield_common.cpp
  src/gameplay/object_cleanup.cpp
  src/persistence/extra_persistence.cpp
  src/effects/adjust_city_yield_modifier_per_population.cpp
  src/effects/adjust_city_yield_modifier_per_suzerain.cpp
  src/effects/adjust_player_strength_modifier_per_suzerain.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000SovietUnionPlugin PROPERTIES
  OUTPUT_NAME "Plugin_YKKZ000_Soviet_Union"
  PREFIX "")

target_include_directories(YKKZ000SovietUnionPlugin PRIVATE include)

target_link_libraries(YKKZ000SovietUnionPlugin PRIVATE ykkz000_bridge)

if(DISABLE_CUSTOM_BEHAVIOR)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_CUSTOM_BEHAVIOR)
endif()

if(DISABLE_STRENGTH_PER_SUZERAIN)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_STRENGTH_PER_SUZERAIN)
endif()

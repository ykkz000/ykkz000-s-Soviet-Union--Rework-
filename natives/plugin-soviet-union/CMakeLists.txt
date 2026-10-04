project(YKKZ000SovietUnionPlugin LANGUAGES CXX)

add_library(YKKZ000SovietUnionPlugin SHARED
  src/plugin.cpp
  src/effects/effect_modules.cpp
  src/effects/engine_access.cpp
  src/effects/city_yield_common.cpp
  src/effects/extra_persistence.cpp
  src/effects/adjust_city_yield_per_population_modifier.cpp
  src/effects/adjust_city_yield_modifier_per_suzerain.cpp
  src/effects/adjust_player_strength_per_suzerain_modifier.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000SovietUnionPlugin PROPERTIES
  OUTPUT_NAME "Plugin_YKKZ000_Soviet_Union"
  PREFIX "")

target_link_libraries(YKKZ000SovietUnionPlugin PRIVATE ykkz000_bridge)

if(DISABLE_CUSTOM_BEHAVIOR)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_CUSTOM_BEHAVIOR)
endif()

if(DISABLE_STRENGTH_PER_SUZERAIN)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE DISABLE_STRENGTH_PER_SUZERAIN)
endif()

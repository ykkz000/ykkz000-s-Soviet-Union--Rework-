project(YKKZ000SovietUnionPlugin LANGUAGES CXX)

add_library(YKKZ000SovietUnionPlugin SHARED
  src/plugin.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000SovietUnionPlugin PROPERTIES
  OUTPUT_NAME "Plugin_YKKZ000_Soviet_Union"
  PREFIX "")

target_link_libraries(YKKZ000SovietUnionPlugin PRIVATE ykkz000_bridge)

if(YKKZ000_DISABLE_CUSTOM_BEHAVIOR)
  target_compile_definitions(YKKZ000SovietUnionPlugin PRIVATE YKKZ000_DISABLE_CUSTOM_BEHAVIOR)
endif()

project(YKKZ000SovietUnionPlugin LANGUAGES CXX)

add_library(YKKZ000SovietUnionPlugin SHARED
  src/plugin.cpp
  plugin.def)

set_target_properties(YKKZ000SovietUnionPlugin PROPERTIES
  OUTPUT_NAME "Plugin_YKKZ000_Soviet_Union"
  PREFIX "")

target_link_libraries(YKKZ000SovietUnionPlugin PRIVATE ykkz000_bridge)

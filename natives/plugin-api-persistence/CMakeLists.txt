project(YKKZ000PluginApiPersistence LANGUAGES CXX)

add_library(YKKZ000PluginApiPersistence SHARED
  src/persistence_api.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000PluginApiPersistence PROPERTIES
  OUTPUT_NAME "ykkz000_plugin_api_persistence"
  PREFIX "")

# PUBLIC include dir: consumers inherit the API header (persistence_api.h).
target_include_directories(YKKZ000PluginApiPersistence PUBLIC include)

target_link_libraries(YKKZ000PluginApiPersistence PRIVATE ykkz000_bridge)

project(YKKZ000PluginApiEffectType LANGUAGES CXX)

add_library(YKKZ000PluginApiEffectType SHARED
  src/plugin.cpp
  src/effecttype_api.cpp
  resource.rc
  plugin.def)

set_target_properties(YKKZ000PluginApiEffectType PROPERTIES
  OUTPUT_NAME "ykkz000_plugin_api_effecttype"
  PREFIX "")

# PUBLIC include dir: consumers inherit the API headers (effect.h, effecttype_api.h, engine_access.h).
target_include_directories(YKKZ000PluginApiEffectType PUBLIC include)

target_link_libraries(YKKZ000PluginApiEffectType PRIVATE ykkz000_bridge)

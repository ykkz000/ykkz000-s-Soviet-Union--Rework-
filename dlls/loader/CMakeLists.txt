project(YKKZ000Loader LANGUAGES CXX)

add_library(YKKZ000Loader SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/gamecore.cpp
  src/registry.cpp
  src/vtable_clone.cpp
  src/plugin_manager.cpp
  loader.def)

set_target_properties(YKKZ000Loader PROPERTIES
  OUTPUT_NAME "GameCore_YKKZ000_Loader_XP2"
  PREFIX "")

target_link_libraries(YKKZ000Loader PRIVATE ykkz000_bridge user32 advapi32)

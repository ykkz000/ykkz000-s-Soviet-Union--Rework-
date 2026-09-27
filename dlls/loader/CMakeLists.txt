project(YKKZ000Loader LANGUAGES CXX)

add_library(YKKZ000Loader SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/gamecore.cpp
  src/registry.cpp
  src/vtable_clone.cpp
  src/custom_effect.cpp
  src/effect_handler.cpp
  src/population_hook.cpp
  src/crash_capture.cpp
  src/plugin_manager.cpp
  loader.def)

set_target_properties(YKKZ000Loader PROPERTIES
  OUTPUT_NAME "GameCore_YKKZ000_Loader_XP2"
  PREFIX "")

target_link_libraries(YKKZ000Loader PRIVATE ykkz000_bridge minhook user32 advapi32 shell32 ole32)

if(YKKZ000_DISABLE_POPULATION_HOOK)
  target_compile_definitions(YKKZ000Loader PRIVATE YKKZ000_DISABLE_POPULATION_HOOK)
endif()

if(YKKZ000_DISABLE_EFFECT_HANDLER)
  target_compile_definitions(YKKZ000Loader PRIVATE YKKZ000_DISABLE_EFFECT_HANDLER)
endif()

if(YKKZ000_DISABLE_HANDLER_GUARD)
  target_compile_definitions(YKKZ000Loader PRIVATE YKKZ000_DISABLE_HANDLER_GUARD)
endif()

if(YKKZ000_ENABLE_DISPATCH_TRACE)
  target_compile_definitions(YKKZ000Loader PRIVATE YKKZ000_ENABLE_DISPATCH_TRACE)
endif()

cmake_minimum_required(VERSION 3.21)
project(YKKZ000Dlls LANGUAGES CXX)

# 诊断用二分开关（默认关闭，仅用于定位崩溃归属）：
# - 关闭自定义行为路径（注册时退化为 kInherit）
# - 禁用 ChangePopulation hook
option(YKKZ000_DISABLE_CUSTOM_BEHAVIOR
  "注册效果时改用 kInherit（禁用自定义效果路径）" OFF)
option(YKKZ000_DISABLE_POPULATION_HOOK
  "禁用 ChangePopulation hook" OFF)
option(YKKZ000_DISABLE_EFFECT_HANDLER
  "禁用效果 handler 注册（内建 handler 表 hook）" OFF)
option(YKKZ000_DISABLE_HANDLER_GUARD
  "禁用 handler 派发守卫（仅供对照复现崩溃）" OFF)
option(YKKZ000_ENABLE_DISPATCH_TRACE
  "启用 0x979290 通用派发 trace hook（诊断用，默认关）" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "仅支持 x64 构建")
endif()

if(MSVC)
  # 静态 CRT：消除 VCRUNTIME140_1.dll 依赖，避免代理 DLL 在缺少新版
  # VC++ 运行库时导致游戏无法启动。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
endif()

# 便于统一取产物（非部署）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# 共享头：INTERFACE 目标，通过链接传播 include 目录
add_library(ykkz000_bridge INTERFACE)
target_include_directories(ykkz000_bridge INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(ykkz000_bridge INTERFACE cxx_std_20)

add_subdirectory(third_party/minhook)
add_subdirectory(loader)
add_subdirectory(plugin-soviet-union)

cmake_minimum_required(VERSION 3.21)
project(YKKZ000Dlls LANGUAGES CXX)

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

add_subdirectory(loader)
add_subdirectory(plugin)

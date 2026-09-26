# MinHook 1.3.4 (BSD-2-Clause) —— 仅供 Loader 使用的内联 hook 库。
# 仅编译 x64 所需源码；沿用顶层设置的静态 CRT（/MT）。
enable_language(C)

add_library(minhook STATIC
  src/buffer.c
  src/hook.c
  src/trampoline.c
  src/hde/hde64.c)

target_include_directories(minhook PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(minhook PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

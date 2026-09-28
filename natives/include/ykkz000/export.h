#pragma once

// 插件导出宏。Loader 的导出在 proxy.cpp 中实现，并由 loader.def 指定
// 序号（155/156/157/158），因此不需要对应的 dllexport 分支。
#define YKKZ000_PLUGIN_API extern "C" __declspec(dllexport)

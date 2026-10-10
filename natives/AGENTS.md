# Agent Instructions for native subproject of ykkz000's Soviet Union (Rework)

## 1. 项目概述

本子项目是一个使用C++为ykkz000's Soviet Union (Rework)项目提供更深层次功能的项目，通过构建Plugin DLL提供扩展功能，并构建Loader DLL
加载原版GameCore DLL（`GameCore_XP2_FinalRelease.dll`）以及Plugin DLL以实现在《文明6》中提供类似原生实现的功能。

- **技术栈**: C++, MSVC, Hook, CMake, vcpkg
- **架构**: DLL，游戏模组（无法独立运行，需要在游戏内加载）

## 2. 项目目录

项目目录为根项目目录下的`natives`目录

## 3. 测试

项目无法在脱离游戏本体的环境下允许，故无自动测试

## 4. 项目风格与规范

- 所有风格和规范**均不对第三方代码和文件生效**
- 所有注释使用美式英文

### 第三方依赖规范

- 第三方依赖**必须**支持`x86_64`（或`x64`、`amd64`等）框架、`win`（Windows）平台
- 第三方依赖的链接方式**必须**为静态链接
- 第三方依赖**优先**使用vcpkg管理依赖，若无法使用vcpkg管理依赖则将其放置于`third_party`目录，例如AAA第三方依赖无法使用vcpkg管理时则放置于`third_party/AAA`

### C++ 代码风格与规范

- 对于由头文件定义的对外暴露的函数和类型需要使用Doxygen注释
- 对于类中`public`和`protected`成员变量和成员方法需要使用Doxygen注释
- 代码应当解耦，不应当出现属于特定模块的逻辑的代码出现在其它模块
- 所有日志输出使用美式英文
- 编译的二进制文件始终携带Copyright信息和许可协议信息（许可协议同项目许可协议，仅提供许可协议名即可）

#### 宏规范

- **优先**使用C++预定义宏、编译器预定义宏和业界常用宏
- 对于由头文件定义的作为API的宏统一使用`YKKZ000_`前缀，而在编译时传入的宏则不使用`YKKZ000_`前缀

#### 日志规范

- 日志统一使用log4cxx
- 日志文件名**禁止**携带时间戳等信息，且日志文件**必须**在每次运行时清空

#### 公共头文件规范

- 所有项目定义的类型和结构均在`ykkz000`命名空间
- 单一头文件**只能**涉及一个命名空间内的函数、类型等定义
- 所有公共头文件的路径结构**必须**与命名空间结构相同

##### Civ6数据结构规范

- 定义：Civ6数据结构指在游戏的GameCore DLL中定义和使用的数据结构
- Civ6数据结构**必须**位于`ykkz000::civ6`命名空间中

##### 扩展数据结构规范

- 定义：扩展数据结构指在基于游戏的GameCore DLL中定义和使用的数据结构额外为其扩展更多字段的数据结构，Civ6数据结构有相同的组织结构
- 扩展数据结构**必须**位于`ykkz000::extra`命名空间中
- 扩展数据结构**必须**命名为`<Civ6数据结构名称>Extra`，如扩展`Player`信息的扩展数据结构命名为`PlayerExtra`
- 对于希望为包含指向B数据结构的A数据结构提供B扩展数据结构，则需要同时实现AExtra和BExtra，在AExtra中以原版映射方式（一对一、一对多、多对一、多对多）组织指向BExtra的字段（使用指针）

#### Loader 代码规范

- Loader**必须**始终保持与游戏的GameCore DLL在游戏中被调用时的兼容性
- Loader**必须**加载游戏的GameCore DLL，并在此基础上应用插件的修改，**禁止**直接替代游戏引擎
- Loader**禁止**实现与修改游戏玩法相关的逻辑，与游戏玩法相关的逻辑均由插件修改

#### Plugin 代码规范

- 对于游戏内容相关的具体实现**禁止**放在主入口，**必须**提供单独的头文件和源文件
- 对于DLL导出的符号，应当使用`export.h`中的`YKKZ000_PLUGIN_API`宏（展开为`extern "C" __declspec(dllexport)`）导出

##### 插件清单与依赖规范

- 每个插件**必须**导出`GetPluginManifest`（返回`ykkz000::bridge::PluginManifest`的常驻POD），声明唯一逻辑名`name`、依赖插件名数组`dependencies`、以及所需的最低Host API版本`requiredHostApi`
- 插件**必须**通过`PluginManifest.name`标识自身；该名称在依赖拓扑中唯一，**禁止**依赖文件名或扫描顺序
- 依赖关系**必须**按逻辑名声明；Loader 会校验缺失依赖、重复名称、依赖环与`requiredHostApi`，并在初始化前按依赖拓扑排序
- 初始化遵循**两阶段**：先加载全部候选DLL，再按拓扑序调用`GetPlugin`；因此依赖插件（如 API 插件）先初始化，consumer 后初始化
- 卸载按**逆拓扑序**：consumer 先卸载，其依赖的 API 插件后卸载
- 插件**必须**保持向后兼容：`PluginManifest`仅可追加字段，已发布字段的含义**禁止**更改

##### API 插件规范

- 共享插件 API（如`ykkz000_plugin_api_effecttype`、`ykkz000_plugin_api_persistence`）本身是插件：它们导出`GetPlugin`/`DestroyPlugin`/`GetPluginManifest`，由Loader 作为插件初始化以捕获`Host`
- API 插件**禁止**包含与游戏玩法相关的具体逻辑；它们只提供通用的、与玩法无关的服务（如引擎访问、持久化、生命周期通知）
- API 通过C-ABI的版本化入口导出（如`GetEffectTypeApi(uint32)`、`GetPersistenceApi(uint32)`），返回**只增不改**的POD函数表；**禁止**跨DLL边界传递STL类型、异常或由分配器拥有的对象
- consumer **必须**在清单中声明对 API 插件的依赖，使其先于自身初始化，并直接`import` API入口函数

##### EffectType 定义代码规范

- 定义EffectType的代码的源文件**必须**放在plugin的源代码目录的`src/effects`目录中，并以该EffectType的去除前两个单词的小写下划线拼写命名（允许长命名）
- EffectType的入口统一声明在plugin的`include/ykkz000/plugin/effects.h`中，并且应当只暴露其它Plugin代码需要的部分，不要暴露内部实现
- EffectType的入口函数**必须**命名为`Get<该EffectType去除前两个单词的PascalCase>Effect`（允许长命名），并返回`const Effect*`

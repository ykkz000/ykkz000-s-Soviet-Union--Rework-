# Agent Instructions for ykkz000's Soviet Union (Rework)

## 1. 项目概述
ykkz000's Soviet Union (Rework)是一个使用Lua、XML和SQL（SQLite方言）组成的《Sid Meier's Civilization VI》（以下简写为“文明6”）游戏的模组。Lua脚本用于扩展功能，XML文件、SQL文件则用于在数据库中定义相关数据。

- **技术栈**: Lua, SQLite
- **架构**: 游戏模组，无法独立运行，需要在游戏内加载

## 2. 环境搭建与构建
无需搭建任何环境，仅依赖于游戏本身

## 3. 测试
无法独立运行，因此无法进行自动测试

## 4. 代码风格与规范
- 所有风格和规范**均不对第三方代码和文件（`**/third_party/*`）生效**
- 所有注释使用美式英文
### C++ 代码风格
- 对于由头文件定义的对外暴露的函数和类型需要使用Doxygen注释
- 对于类中`public`和`protected`成员变量和成员方法需要使用Doxygen注释
- 代码应当解耦，不应当出现属于特定模块的逻辑的代码出现在其它模块
- 所有日志输出使用美式英文
- 编译的二进制文件始终携带Copyright信息和许可协议信息（许可协议同项目许可协议，仅提供许可协议名即可）
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
##### EffectType 定义代码规范
- 定义EffectType的代码的头文件和源文件**必须**放在plugin的源代码目录的`effects`目录中
- 定义EffectType的代码的头文件和源文件**必须**以该EffectType的去除前两个单词的小写下划线拼写命名（允许长命名）
- 定义EffectType的代码的头文件应当只暴露其它Plugin代码需要的部分，不要暴露内部实现

### Lua 代码风格
- 代码顺序依次为：类型定义、工具函数定义、事件HOOK函数定义、事件注册、其它需要在加载时执行的代码
- 对于工具函数，应当使用小写且以`__`开头（例如`__has_trait`）且需添加`local`关键字避免冲突
- 对于事件HOOK函数定义应当命名为`<特性>_<具体功能>`（均为大写），然后将其注册到事件监听器中

### XML 风格
- 所有XML文件以`<?xml version="1.0" encoding="utf-8"?>`开头
- 所有XML文件（除`ykkz000's Soviet Union (Rework).civ6proj`）的根标签为无属性的`<GameData>`标签
- 所有XML文件的缩进为2空格
- 对于`Data`目录下的XML文件的`<Row>` 标签，建议使用属性而非嵌套标签格式，例如：
```xml
<!--建议的格式-->
<Row Type="UNIT_YKKZ000_RED_ARMY" Kind="KIND_UNIT" />

<!--不建议的格式-->
<Row>
  <Type>UNIT_YKKZ000_RED_ARMY</Type>
  <Kind>KIND_UNIT</Kind>
</Row>
```
- 对于`Text`目录下的XML文件，建议使用`<Row>`标签而不使用`<Replace>`标签，例如：
```xml
<!--建议的格式-->
<Row Tag="LOC_EXAMPLE" Language="zh_Hans_CN">
  <Text>EXAMPLE</Text>
</Row>

<!--不建议的格式-->
<Replace Tag="LOC_EXAMPLE" Language="zh_Hans_CN">
  <Text>EXAMPLE</Text>
</Replace>
```
- 对于`Text\en_US`目录下的XML文件，建议使用`<BaseGameText>`标签而不使用`<LocalizedText>`标签
 
### SQL 风格
- 所有SQL文件均使用SQLite方言的语法
- 所有SQL关键字均使用大写

## 5. Git 规则
- **提交信息**: 遵循 Conventional Commits
- **提交控制**：仅暂存流程中的修改的文件，提交后上传到远程仓库

## 6. 安全与重要约束
- **允许修改**: `.tools/`、`natives/`、`Data/`、`Text/`、`Scripts/`、`ykkz000's Soviet Union (Rework).civ6proj`
- **禁止修改**: 除允许修改的列表以外的任何目录和文件
- **添加/删除文件**: 在添加或删除文件/目录后，同步更新`ykkz000's Soviet Union (Rework).civ6proj`文件的`<ItemGroup>`标签内容。另外判断文件在游戏中的加载时机以及作用，更新`<FrontEndActionData>`标签或`<InGameActionData>`标签的内容

## 7. 参考手册与目录
- 文明6游戏wiki参考：[文明百科-文明VI](https://www.civilopedia.net/zh-CN/gathering-storm/concepts/intro/)
- 文明6Lua参考：[Index - Civilization VI: Modding Knowledge Base](https://sukritact.github.io/Civilization-VI-Modding-Knowledge-Base/)
- 文明6数据库参考：[笑笑/文明6mod教学 - 码云 - 开源中国](https://gitee.com/xiaoxiaoccat/civ-6-mod-tutorial/tree/master)
- 文明6运行时日志：`%LocalAppData%\Firaxis Games\Sid Meier's Civilization VI/Logs`
- 文明6运行时数据库：`%LocalAppData%\Firaxis Games\Sid Meier's Civilization VI/Cache`

## 8. 常见问题与陷阱
- VSCode检测到Lua文件存在外部函数定义和调用的冲突，如参数数量不匹配等：这是VSCode的一个插件的Bug，游戏定义的函数以参考手册为主
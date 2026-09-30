# ykkz000的苏联（重做版）

[English](README.md)

添加新文明 ***苏联*** 和新领袖 ***弗拉基米尔·列宁*** 与 ***约瑟夫·斯大林***。

支持的语言：English、简体中文。

## 依赖

- 需《席德·梅尔的文明 VI》的**风云变幻（Expansion 2）**规则集。本模组仅支持该规则集（见 `.civ6proj` 的 `ActionCriteriaData` 中的 `RuleSetInUse=RULESET_EXPANSION_2`）。

## 游戏中的效果

### 文明：苏联

- **各尽所能，按劳分配**：所有城市每人口提供 0.5 生产力，并每人口提供 +5% 生产力；无法建造圣地、无法招募大预言家、无法创立宗教。
- **集体农庄**（特色区域，替代“工业区”）：解锁科技为采矿业而非学徒制，且建造花费更低；提供 1 住房；公民收益 +1 食物、+1 生产力；生产力相邻加成也能提供食物；每与一个矿山相邻 +1 生产力、每与一个战略资源相邻 +2 生产力；每与一个区域相邻 +1 生产力，并为相邻区域提供标准相邻加成；此城市的工业区建筑额外提供等同于其生产力的食物和 1 住房。
- **红军**（特色单位，替代步兵，现代时代）：不消耗资源；在本国领土中 +3 战斗力；每与 1 个红军相邻 +1 战斗力。

### 领袖：弗拉基米尔·列宁

- **和平法令**：影响力点数 +50%；每宗主一个城邦，所有城市 +5% 生产力和食物。
- **和平法令（风云变幻）**额外：无法宣布突袭战争、无法对城邦宣战、无法使用“圣战”“殖民战争”“领土扩张战争”战争借口；来自城邦的外交支持 +100%。
- **议程：国际主义**——喜欢和平的文明，讨厌处于战争的文明。

### 领袖：约瑟夫·斯大林

- **大纵深作战理论**：近战单位击杀单位后恢复移动力和攻击次数；所有近战单位 +1 移动力、+5 战斗力；每击杀 3 个单位获得 1 个使者（所有击杀均计入计数，但只有近战单位获得移动力与攻击次数恢复）；每宗主一个城邦，所有单位 +1 战斗力。
- **议程：社会沙文主义**——喜欢低战斗力的文明，讨厌高战斗力的文明。

## 构建环境配置

- Windows x64。
- PowerShell 5.1 或更高版本（`.tools/build.ps1` 以 `#Requires -Version 5.1` 开头）。
- CMake 3.21 或更高版本（`natives/CMakeLists.txt` 中的 `cmake_minimum_required(VERSION 3.21)`）。
- MSVC 的 v143 工具集、C++20、x64。可使用 `natives/CMakePresets.json` 提供的 `x64-v143`、`x64-ninja` preset 手动进行 CMake 配置。
- 《文明 VI》SDK 与 SDK Assets。默认使用 Steam 默认路径，可用 `--base-sdk` / `--base-assets` 覆盖：
  - `...\steamapps\common\Sid Meier's Civilization VI SDK`
  - `...\steamapps\common\Sid Meier's Civilization VI SDK Assets`
- MinHook 已随仓库内置（`natives/third_party/minhook`），无需额外获取。

## 从源码构建

使用 `.tools/build.ps1` 构建。项目文件路径包含空格和括号，因此需要加引号：

```powershell
# 完整构建（DLL + 资源 + 内容），部署到默认 Mods 目录
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj"

# 仅改文本/数据（未改 native 代码与资源）
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip="dll,art"

# 未改资源
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip=art

# 未改 native 代码
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip=dll

# 删除产物与 DLL 构建目录后重建（删除过文件时使用）
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --clean
```

- 默认输出：`<Documents>\My Games\Sid Meier's Civilization VI\Mods\ykkz000's Soviet Union (Rework)`；可用 `-o <path>` / `--output <path>` 覆盖。
- 部署产物：loader 部署为 `Binaries/Win64/GameCore_YKKZ000_Loader_XP2_FinalRelease.dll`，plugin 部署到 `Binaries/Win64/ykkz000_civ6_plugin/Plugin_YKKZ000_Soviet_Union.dll`。`--dll-prefix` 必须与 `Data/YKKZ000_GameCores.sql` 中的 `DllPrefix` 一致。

## 许可证

本项目在 MIT 许可证下授权（见 `LICENSE`）。该许可证仅适用于源代码；多媒体资源与第三方组件分别见 `NOTICE` 与 `THIRD_PARTY_NOTICES.txt`。

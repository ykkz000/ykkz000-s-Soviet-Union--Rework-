# ykkz000's Soviet Union (Rework)

[简体中文](README.zh_cn.md)

Adds the new civilization ***Soviet Union*** and the new leaders ***Vladimir Lenin*** and ***Joseph Stalin***.

Supported languages: English, 简体中文.

## Requirements

- *Sid Meier's Civilization VI* with the **Gathering Storm (Expansion 2)** ruleset. The mod only supports this ruleset (see `RuleSetInUse=RULESET_EXPANSION_2` in `ActionCriteriaData` of the `.civ6proj`).

## In-Game Effects

### Civilization: Soviet Union

- **From Each According to His Ability, to Each According To His Work**: every city gains 0.5 Production per Citizen, plus +5% Production per Citizen; may not build Holy Site districts, gain Great Prophets, or found Religions.
- **Kolkhoz** (unique district, replaces the Industrial Zone): available at Mining instead of Apprenticeship, and cheaper to build; provides 1 Housing; Citizen Yields of +1 Food and +1 Production; the Production adjacency bonus also provides Food; +1 Production from an adjacent Mine and +2 Production from an adjacent strategic resource; +1 Production for each adjacent district and provides a standard adjacency bonus to adjacent districts; Industrial Zone buildings in this city also provide Food equal to their Production and 1 Housing.
- **Red Army** (unique unit, replaces Infantry, Modern era): costs no resource; +3 Combat Strength when fighting in home territory; +1 Combat Strength for every adjacent Red Army.

### Leader: Vladimir Lenin

- **Decree on Peace**: +50% Influence Points; all cities gain +5% Production and Food for each city-state you are Suzerain of.
- **Decree on Peace (Gathering Storm)**, in addition: cannot declare Surprise Wars, cannot declare war on a city-state, and cannot use the Holy War, Colonial War, or Territorial Expansion War casus belli; +100% Diplomatic Favor from Suzerain city-states.
- **Agenda: International** — likes civilizations that are at peace and hates civilizations that are at war.

### Leader: Joseph Stalin

- **Grand Depth Operational Theory**: melee units restore Movement and attacks after killing; all melee units gain +1 Movement and +5 Combat Strength; gain 1 Envoy after every 3 kills (all kills count toward the counter, while only melee units get the Movement and attack restoration); all units gain +1 Combat Strength for each city-state you are Suzerain of.
- **Agenda: Social Chauvinism** — likes civilizations with low combat strength and hates civilizations with high combat strength.

## Build Environment

- Windows x64.
- PowerShell 5.1 or later (`.tools/build.ps1` starts with `#Requires -Version 5.1`).
- CMake 3.21 or later (`cmake_minimum_required(VERSION 3.21)` in `natives/CMakeLists.txt`).
- MSVC with the v143 toolset, C++20, x64. The provided `natives/CMakePresets.json` presets (`x64-v143`, `x64-ninja`) can be used for a manual CMake configure.
- *Civilization VI* SDK and SDK Assets. The default Steam paths are used unless overridden with `--base-sdk` / `--base-assets`:
  - `...\steamapps\common\Sid Meier's Civilization VI SDK`
  - `...\steamapps\common\Sid Meier's Civilization VI SDK Assets`
- MinHook is bundled in the repository (`natives/third_party/minhook`) and does not need to be obtained separately.

## Building from Source

Build with `.tools/build.ps1`. Quote the project file because its path contains spaces and parentheses:

```powershell
# Full build (DLLs + art + content), deployed to the default Mods directory
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj"

# Only text/data changed (native code and art unchanged)
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip="dll,art"

# Art unchanged
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip=art

# Native code unchanged
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --skip=dll

# Delete the output and DLL build directories, then rebuild (use after deleting files)
& ".\.tools\build.ps1" "ykkz000's Soviet Union (Rework).civ6proj" --clean
```

- Default output: `<Documents>\My Games\Sid Meier's Civilization VI\Mods\ykkz000's Soviet Union (Rework)`; override with `-o <path>` / `--output <path>`.
- Deployed artifacts: the loader as `Binaries/Win64/GameCore_YKKZ000_Loader_XP2_FinalRelease.dll` and the plugin as `Binaries/Win64/ykkz000_civ6_plugin/Plugin_YKKZ000_Soviet_Union.dll`. The `--dll-prefix` must match the `DllPrefix` in `Data/YKKZ000_GameCores.sql`.

## License

Licensed under the MIT License (see `LICENSE`). The license applies to the source code only; multimedia resources and third-party components are covered by `NOTICE` and `THIRD_PARTY_NOTICES.txt`.

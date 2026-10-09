#Requires -Version 5.1

# Opens the Civilization VI Asset Editor (AssetEditor.exe) for this mod project
# without launching ModBuddy, mirroring what ModBuddy's "Tools > Asset Editor"
# command does.
#
# The argument contract below was verified against a real ModBuddy launch. The
# editor's usage text is:
#   Usage: AssetEditor.exe PathToGameInstall ModName Mod PantryPath
#          [DependencyName DependencyPantryPath...]
# The real layout is:
#   1. <BaseSdk>\AssetModTools\Cooker   (the directory containing Civ6.cfg; the
#      "PathToGameInstall" label is misleading)
#   2. <ProjectName>                    (the .civ6proj base name)
#   3. <ProjectDir>                     (the mod pantry)
#   4+. <DependencyName> <DependencyPantryPath> pairs, resolved from the mod
#       .Art.xml requiredGameArtIDs in parent-before-child (pre-order).
#
# If a future SDK/editor update changes the command line, re-capture it from a
# real launch and adjust Get-AssetEditorArgumentList/Resolve-Dependencies:
#   1. In ModBuddy, open this project and run Tools > Asset Editor once.
#   2. Capture the real command line from another shell:
#        Get-CimInstance Win32_Process -Filter "Name='AssetEditor.exe'" |
#          Select-Object -ExpandProperty CommandLine
#   3. Compare it with:
#        .\open_asset_editor.ps1 "<project>" --dry-run

function Get-UsageText {
  return @"
Usage: open_asset_editor.ps1 <project.civ6proj> [options]

Opens the Civilization VI Asset Editor (AssetEditor.exe) for the given mod
project, without launching ModBuddy. The editor receives the Cooker directory,
the project base name, the project directory, and the dependency name/path pairs
resolved from the project's .Art.xml in pre-order, matching ModBuddy's
"Tools > Asset Editor" command.

Arguments:
  <project.civ6proj>        Path to the project file to open (required).

Options:
  --base-assets <path>      Civilization VI SDK Assets root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK Assets
  --base-sdk <path>         Civilization VI SDK root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK
  --extra-arg <arg>         Extra argument appended verbatim to the command
                            line; may be repeated. Use --extra-arg=<arg> for
                            values that start with '-'.
                            Default: (none)
  --no-default-args         Do not pass the reconstructed project/pantry
                            arguments; launch with only --extra-arg values
                            (manual calibration escape hatch).
  -w, --wait                Wait for the editor process to exit.
                            Default: return immediately.
  --dry-run                 Print the resolved exe and the full argument list,
                            then exit without launching.
  -h, --help                Show this help and exit.

Examples:
  .\open_asset_editor.ps1 "ykkz000's Soviet Union (Rework).civ6proj"
  .\open_asset_editor.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --dry-run
  .\open_asset_editor.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --wait
  .\open_asset_editor.ps1 "ykkz000's Soviet Union (Rework).civ6proj" --no-default-args --extra-arg="--foo"
"@
}

function Parse-Args {
  param([string[]]$ArgList)

  if ($null -eq $ArgList) { $ArgList = @() }

  $opts = [ordered]@{
    Input         = $null
    BaseAssets    = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK Assets"
    BaseSdk       = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK"
    ExtraArgs     = [System.Collections.Generic.List[string]]::new()
    NoDefaultArgs = $false
    Wait          = $false
    DryRun        = $false
    Help          = $false
  }

  $i = 0
  while ($i -lt $ArgList.Count) {
    $a = $ArgList[$i]
    if ($a -eq '-h' -or $a -eq '--help') {
      $opts.Help = $true
      $i++
    }
    elseif ($a -eq '--base-assets') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.BaseAssets = $v
      $i += 2
    }
    elseif ($a -like '--base-assets=*') {
      $v = $a.Substring('--base-assets='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--base-assets' requires a value." }
      $opts.BaseAssets = $v
      $i++
    }
    elseif ($a -eq '--base-sdk') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.BaseSdk = $v
      $i += 2
    }
    elseif ($a -like '--base-sdk=*') {
      $v = $a.Substring('--base-sdk='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--base-sdk' requires a value." }
      $opts.BaseSdk = $v
      $i++
    }
    elseif ($a -eq '--extra-arg') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.ExtraArgs.Add($v)
      $i += 2
    }
    elseif ($a -like '--extra-arg=*') {
      $v = $a.Substring('--extra-arg='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--extra-arg' requires a value." }
      $opts.ExtraArgs.Add($v)
      $i++
    }
    elseif ($a -eq '--no-default-args') {
      $opts.NoDefaultArgs = $true
      $i++
    }
    elseif ($a -eq '-w' -or $a -eq '--wait') {
      $opts.Wait = $true
      $i++
    }
    elseif ($a -eq '--dry-run') {
      $opts.DryRun = $true
      $i++
    }
    elseif ($a.Length -gt 1 -and $a[0] -eq '-') {
      throw "Unknown option: $a"
    }
    else {
      if ($null -ne $opts.Input) { throw "Unexpected argument: $a" }
      $opts.Input = $a
      $i++
    }
  }

  return [pscustomobject]$opts
}

function Resolve-FullPath {
  param([string]$Path)
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return [System.IO.Path]::GetFullPath($Path)
  }
  $base = (Get-Location).Path
  return [System.IO.Path]::GetFullPath((Join-Path $base $Path))
}

function Remove-TrailingSeparator {
  param([string]$Path)
  if ([string]::IsNullOrEmpty($Path)) { return $Path }
  $full = [System.IO.Path]::GetFullPath($Path)
  if ($full -eq [System.IO.Path]::GetPathRoot($full)) { return $full }
  return $full.TrimEnd('\', '/')
}

function Read-ArtXmlInfo {
  param([string]$Path)

  $text = [System.IO.File]::ReadAllText($Path)

  $name = ''
  $nameMatch = [regex]::Match($text, '<name\s+text="([^"]*)"')
  if ($nameMatch.Success) { $name = $nameMatch.Groups[1].Value }

  $id = ''
  $idMatch = [regex]::Match($text, '<id\s+text="([^"]*)"')
  if ($idMatch.Success) { $id = $idMatch.Groups[1].Value }

  $required = New-Object System.Collections.Generic.List[string]
  $reqMatch = [regex]::Match($text, '<requiredGameArtIDs>(.*?)</requiredGameArtIDs>', [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if ($reqMatch.Success) {
    foreach ($m in [regex]::Matches($reqMatch.Groups[1].Value, '<id\s+text="([^"]*)"')) {
      $required.Add($m.Groups[1].Value)
    }
  }

  return [pscustomobject]@{ Name = $name; Id = $id; Required = $required }
}

function Resolve-Dependencies {
  param([string]$ProjectDir, [string]$BaseAssets, $RootRequired)

  if (-not (Test-Path -LiteralPath $BaseAssets -PathType Container)) {
    throw "Base assets path not found: $BaseAssets"
  }

  try {
    $candidates = @(Get-ChildItem -LiteralPath $BaseAssets -Recurse -Filter *.Art.xml -File -ErrorAction Stop)
  }
  catch {
    throw "Failed to scan base assets at '$BaseAssets': $($_.Exception.Message)"
  }

  $map = @{}
  foreach ($file in $candidates) {
    if ($file.Directory.Name -ine 'pantry') { continue }
    try {
      $info = Read-ArtXmlInfo $file.FullName
    }
    catch {
      throw "Failed to read pantry file '$($file.FullName)': $($_.Exception.Message)"
    }
    if (-not [string]::IsNullOrEmpty($info.Id) -and -not $map.ContainsKey($info.Id)) {
      $map[$info.Id] = [pscustomobject]@{ Name = $info.Name; Dir = $file.Directory.FullName; Required = $info.Required }
    }
  }

  $result = New-Object System.Collections.Generic.List[object]
  $ctx = @{ Map = $map; Visited = @{}; Result = $result; BaseAssets = $BaseAssets }

  function Visit-ArtId {
    param([string]$Id, $Ctx)
    if ($Ctx.Visited.ContainsKey($Id)) { return }
    $Ctx.Visited[$Id] = $true
    if (-not $Ctx.Map.ContainsKey($Id)) {
      throw "Required game art ID '$Id' was not found in any pantry under '$($Ctx.BaseAssets)'."
    }
    $entry = $Ctx.Map[$Id]
    $Ctx.Result.Add([pscustomobject]@{ Name = $entry.Name; Dir = $entry.Dir })
    foreach ($child in $entry.Required) { Visit-ArtId -Id $child -Ctx $Ctx }
  }

  foreach ($id in $RootRequired) { Visit-ArtId -Id $id -Ctx $ctx }

  return $result
}

function Assert-Inputs {
  param($opts)

  $inputPath = Resolve-FullPath $opts.Input
  if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
    throw "Project file not found: $inputPath"
  }
  if (-not $inputPath.EndsWith('.civ6proj', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The input file must have the '.civ6proj' extension: $inputPath"
  }

  $projectDir = Split-Path -Parent $inputPath
  $projectName = [System.IO.Path]::GetFileNameWithoutExtension($inputPath)

  $artXmls = @(Get-ChildItem -LiteralPath $projectDir -Filter ($projectName + '.Art.xml') -File -ErrorAction SilentlyContinue)
  if ($artXmls.Count -eq 0) {
    throw "The project's art file was not found: $(Join-Path $projectDir ($projectName + '.Art.xml'))"
  }
  if ($artXmls.Count -gt 1) {
    throw "More than one '$projectName.Art.xml' file was found in '$projectDir'; exactly one is allowed."
  }

  $baseAssets = Remove-TrailingSeparator (Resolve-FullPath $opts.BaseAssets)
  if (-not (Test-Path -LiteralPath $baseAssets -PathType Container)) {
    throw "Base assets path not found: $baseAssets"
  }

  $baseSdk = Remove-TrailingSeparator (Resolve-FullPath $opts.BaseSdk)
  $exe = Join-Path (Join-Path (Join-Path $baseSdk 'AssetModTools') 'AssetEditor') 'AssetEditor.exe'
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Cannot find the Asset Editor at '$exe' (check --base-sdk)."
  }

  $cookerDir = Join-Path (Join-Path $baseSdk 'AssetModTools') 'Cooker'
  $cfg = Join-Path $cookerDir 'Civ6.cfg'
  if (-not (Test-Path -LiteralPath $cfg -PathType Leaf)) {
    throw "Cannot find the asset config at '$cfg' (check --base-sdk)."
  }

  return [pscustomobject]@{
    InputPath    = $inputPath
    ProjectDir   = $projectDir
    ProjectName  = $projectName
    ArtXml       = $artXmls[0].FullName
    BaseAssets   = $baseAssets
    BaseSdk      = $baseSdk
    CookerDir    = $cookerDir
    Exe          = $exe
    ExtraArgs    = @($opts.ExtraArgs)
    NoDefaultArgs = [bool]$opts.NoDefaultArgs
    Wait         = [bool]$opts.Wait
    DryRun       = [bool]$opts.DryRun
  }
}

# Assembles the arguments passed to AssetEditor.exe.
#
# Layout (verified against a real ModBuddy launch): the Cooker dir, the project
# base name, the project dir, then the resolved dependency name/path pairs in
# pre-order. Each value is quoted here; Invoke-AssetEditor joins the list with
# spaces without adding another quoting layer.
function Get-AssetEditorArgumentList {
  param([string]$CookerDir, [string]$ModName, [string]$ProjectDir, $Dependencies, [string[]]$ExtraArgs, [bool]$NoDefaultArgs)

  $list = New-Object System.Collections.Generic.List[string]

  if (-not $NoDefaultArgs) {
    $list.Add(('"{0}"' -f $CookerDir))
    $list.Add(('"{0}"' -f $ModName))
    $list.Add(('"{0}"' -f $ProjectDir))
    foreach ($dep in $Dependencies) {
      $list.Add(('"{0}"' -f $dep.Name))
      $list.Add(('"{0}"' -f $dep.Dir))
    }
  }

  if ($null -ne $ExtraArgs) {
    foreach ($arg in $ExtraArgs) { $list.Add($arg) }
  }

  return $list
}

function Invoke-AssetEditor {
  param($Ctx, [string[]]$ArgumentList)

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Ctx.Exe
  $psi.Arguments = ($ArgumentList -join ' ')
  $psi.WorkingDirectory = $Ctx.ProjectDir
  $psi.UseShellExecute = $false
  $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Normal

  if ($Ctx.DryRun) {
    [Console]::Out.WriteLine("Exe:       " + $psi.FileName)
    [Console]::Out.WriteLine("Arguments: " + $psi.Arguments)
    return 0
  }

  Write-Host ("Launching " + $Ctx.Exe)
  $proc = [System.Diagnostics.Process]::Start($psi)
  if ($null -eq $proc) {
    throw "Failed to start the Asset Editor."
  }

  if ($Ctx.Wait) {
    $proc.WaitForExit()
    Write-Host ("Asset Editor exited with code {0}." -f $proc.ExitCode)
  }

  return 0
}

function Main {
  param([string[]]$ArgList)

  $opts = Parse-Args -ArgList $ArgList

  if ($opts.Help) {
    [Console]::Out.WriteLine((Get-UsageText))
    return 0
  }

  if ([string]::IsNullOrEmpty($opts.Input)) {
    [Console]::Error.WriteLine((Get-UsageText))
    return 1
  }

  $ctx = Assert-Inputs -opts $opts

  $rootInfo = Read-ArtXmlInfo $ctx.ArtXml
  $dependencies = @(Resolve-Dependencies -ProjectDir $ctx.ProjectDir -BaseAssets $ctx.BaseAssets -RootRequired $rootInfo.Required)

  $argumentList = @(Get-AssetEditorArgumentList -CookerDir $ctx.CookerDir -ModName $ctx.ProjectName -ProjectDir $ctx.ProjectDir -Dependencies $dependencies -ExtraArgs $ctx.ExtraArgs -NoDefaultArgs $ctx.NoDefaultArgs)

  if (-not $ctx.DryRun) {
    Write-Host ("Resolved {0} dependency pantry path(s)." -f $dependencies.Count)
  }

  return (Invoke-AssetEditor -Ctx $ctx -ArgumentList $argumentList)
}

$finalExitCode = 0
try {
  $finalExitCode = Main -ArgList $args
}
catch {
  [Console]::Error.WriteLine("Error: " + $_.Exception.Message)
  $finalExitCode = 1
}
exit $finalExitCode

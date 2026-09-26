#Requires -Version 5.1

function Get-UsageText {
  return @"
Usage: build.ps1 <project.civ6proj> [options]

Builds a Civilization VI mod project without ModBuddy or MSBuild. The project
file is parsed, content files are copied, the Firaxis asset cooker is run, and
the .modinfo file is generated.

Arguments:
  <project.civ6proj>        Path to the project file to build (required).

Options:
  -o, --output <path>       Build output directory.
                            Default: <My Documents>\My Games\Sid Meier's
                            Civilization VI\Mods\<projectName>
      --skip-art            Do not run the asset cooker; reuse the art already
                            present in the output directory.
      --clean               Delete the output directory before building.
      --base-assets <path>  Civilization VI SDK Assets root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK Assets
      --base-sdk <path>     Civilization VI SDK root.
                            Default: C:\Program Files (x86)\Steam\steamapps\
                            common\Sid Meier's Civilization VI SDK
  -h, --help                Show this help and exit.

Examples:
  .\build.ps1 "ykkz000's Soviet Union (Rework).civ6proj"
  .\build.ps1 mod.civ6proj --clean -o "C:\temp\mod-build"
"@
}

function Parse-Args {
  param([string[]]$ArgList)

  if ($null -eq $ArgList) { $ArgList = @() }

  $opts = [ordered]@{
    Input      = $null
    Output     = $null
    SkipArt    = $false
    Clean      = $false
    BaseAssets = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK Assets"
    BaseSdk    = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization VI SDK"
    Help       = $false
  }

  $i = 0
  while ($i -lt $ArgList.Count) {
    $a = $ArgList[$i]
    if ($a -eq '-h' -or $a -eq '--help') {
      $opts.Help = $true
      $i++
    }
    elseif ($a -eq '-o' -or $a -eq '--output') {
      if ($i + 1 -ge $ArgList.Count) { throw "Option '$a' requires a value." }
      $v = $ArgList[$i + 1]
      if ([string]::IsNullOrEmpty($v)) { throw "Option '$a' requires a value." }
      $opts.Output = $v
      $i += 2
    }
    elseif ($a -like '--output=*') {
      $v = $a.Substring('--output='.Length)
      if ([string]::IsNullOrEmpty($v)) { throw "Option '--output' requires a value." }
      $opts.Output = $v
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
    elseif ($a -eq '--skip-art') {
      $opts.SkipArt = $true
      $i++
    }
    elseif ($a -eq '--clean') {
      $opts.Clean = $true
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

function Test-PathUnder {
  param([string]$Child, [string]$Parent)
  $c = [System.IO.Path]::GetFullPath($Child)
  $p = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
  if ([string]::IsNullOrEmpty($p)) { return $true }
  $prefix = $p + [System.IO.Path]::DirectorySeparatorChar
  return $c.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-PathEqual {
  param([string]$A, [string]$B)
  $a = Remove-TrailingSeparator $A
  $b = Remove-TrailingSeparator $B
  return ($a -eq $b)
}

function Assert-Inputs {
  param($opts)

  if ($opts.Clean -and $opts.SkipArt) {
    throw "Options '--clean' and '--skip-art' cannot be used together."
  }

  $inputPath = Resolve-FullPath $opts.Input
  if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
    throw "Project file not found: $inputPath"
  }
  if (-not $inputPath.EndsWith('.civ6proj', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The input file must have the '.civ6proj' extension: $inputPath"
  }

  $projectDir = Split-Path -Parent $inputPath
  $projectName = [System.IO.Path]::GetFileNameWithoutExtension($inputPath)

  $documents = [Environment]::GetFolderPath('MyDocuments')
  if ([string]::IsNullOrEmpty($documents)) {
    $documents = Join-Path $env:UserProfile 'Documents'
  }
  $defaultOut = Remove-TrailingSeparator (Join-Path $documents ("My Games\Sid Meier's Civilization VI\Mods\" + $projectName))

  if ([string]::IsNullOrEmpty($opts.Output)) {
    $out = $defaultOut
  }
  else {
    $out = Remove-TrailingSeparator (Resolve-FullPath $opts.Output)
  }

  return [pscustomobject]@{
    InputPath   = $inputPath
    ProjectDir  = $projectDir
    ProjectName = $projectName
    Out         = $out
    DefaultOut  = $defaultOut
    BaseAssets  = (Resolve-FullPath $opts.BaseAssets)
    BaseSdk     = (Resolve-FullPath $opts.BaseSdk)
    SkipArt     = [bool]$opts.SkipArt
    Clean       = [bool]$opts.Clean
  }
}

function Invoke-Clean {
  param([string]$Out, [string]$ProjectDir, [string]$DefaultOut)

  if ([string]::IsNullOrWhiteSpace($Out)) {
    throw "Refusing to clean: the output path is empty."
  }

  $fullOut = Remove-TrailingSeparator $Out
  if ([string]::IsNullOrEmpty($fullOut)) {
    throw "Refusing to clean: the output path is empty."
  }

  $root = [System.IO.Path]::GetPathRoot($fullOut)
  if (-not [string]::IsNullOrEmpty($root) -and (Test-PathEqual $fullOut $root)) {
    throw "Refusing to clean the filesystem root: $fullOut"
  }

  if (Test-PathEqual $fullOut $ProjectDir) {
    throw "Refusing to clean the project directory: $fullOut"
  }

  if (Test-PathUnder $ProjectDir $fullOut) {
    throw "Refusing to clean '$fullOut' because the project directory is inside it."
  }

  if (-not [string]::IsNullOrEmpty($DefaultOut) -and (Test-PathUnder $DefaultOut $fullOut)) {
    throw "Refusing to clean '$fullOut' because it contains the default mod output directory."
  }

  if (Test-Path -LiteralPath $fullOut) {
    Remove-Item -LiteralPath $fullOut -Recurse -Force -ErrorAction Stop
  }
}

function Read-Project {
  param([string]$Path)

  $doc = New-Object System.Xml.XmlDocument
  try {
    $doc.Load($Path)
  }
  catch {
    throw "Failed to parse the project file '$Path': $($_.Exception.Message)"
  }

  $getText = {
    param([string]$Name)
    $nodes = $doc.GetElementsByTagName($Name)
    if ($nodes.Count -eq 0) { return '' }
    return $nodes[0].InnerText
  }

  $getCdata = {
    param([string]$Name)
    $nodes = $doc.GetElementsByTagName($Name)
    if ($nodes.Count -eq 0) { return $null }
    foreach ($child in $nodes[0].ChildNodes) {
      if ($child.NodeType -eq [System.Xml.XmlNodeType]::CDATA) { return $child.Value }
    }
    return $null
  }

  $content = New-Object System.Collections.Generic.List[string]
  foreach ($node in $doc.GetElementsByTagName('Content')) {
    $inc = $node.GetAttribute('Include')
    if (-not [string]::IsNullOrEmpty($inc)) { $content.Add($inc) }
  }

  return [pscustomobject]@{
    Guid               = (& $getText 'Guid')
    ModVersion         = (& $getText 'ModVersion')
    Name               = (& $getText 'Name')
    Teaser             = (& $getText 'Teaser')
    Description        = (& $getText 'Description')
    Authors            = (& $getText 'Authors')
    CompatibleVersions = (& $getText 'CompatibleVersions')
    SpecialThanks      = (& $getText 'SpecialThanks')
    Homepage           = (& $getText 'Homepage')
    ActionCriteriaData = (& $getCdata 'ActionCriteriaData')
    FrontEndActionData = (& $getCdata 'FrontEndActionData')
    InGameActionData   = (& $getCdata 'InGameActionData')
    LocalizedTextData  = (& $getCdata 'LocalizedTextData')
    AssociationData    = (& $getCdata 'AssociationData')
    Content            = $content
  }
}

function Copy-ContentFiles {
  param($ContentList, [string]$ProjectDir, [string]$Out, [bool]$SkipArt)

  $result = New-Object System.Collections.Generic.List[string]
  foreach ($rel in $ContentList) {
    if ([System.IO.Path]::IsPathRooted($rel)) {
      throw "Content path must be relative to the project directory: $rel"
    }
    if ($SkipArt -and $rel -match '\.artdef$') { continue }

    $src = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $rel))
    if (-not (Test-PathUnder $src $ProjectDir)) {
      throw "Content path escapes the project directory: $rel"
    }
    $dst = [System.IO.Path]::GetFullPath((Join-Path $Out $rel))
    if (-not (Test-PathUnder $dst $Out)) {
      throw "Content path escapes the output directory: $rel"
    }
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
      throw "Content file listed in the project does not exist: $rel"
    }

    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dstDir)) {
      New-Item -ItemType Directory -Path $dstDir -Force -ErrorAction Stop | Out-Null
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
    $result.Add(($rel -replace '\\', '/'))
  }

  return [pscustomobject]@{ Count = $result.Count; Files = $result }
}

function Read-ArtXmlInfo {
  param([string]$Path)

  $text = [System.IO.File]::ReadAllText($Path)

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

  return [pscustomobject]@{ Id = $id; Required = $required }
}

function Resolve-PantryPaths {
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
      $map[$info.Id] = [pscustomobject]@{ Dir = $file.Directory.FullName; Required = $info.Required }
    }
  }

  $result = New-Object System.Collections.Generic.List[string]
  $ctx = @{ Map = $map; Visited = @{}; Result = $result; BaseAssets = $BaseAssets }

  function Visit-ArtId {
    param([string]$Id, $Ctx)
    if ($Ctx.Visited.ContainsKey($Id)) { return }
    $Ctx.Visited[$Id] = $true
    if (-not $Ctx.Map.ContainsKey($Id)) {
      throw "Required game art ID '$Id' was not found in any pantry under '$($Ctx.BaseAssets)'."
    }
    $entry = $Ctx.Map[$Id]
    foreach ($child in $entry.Required) { Visit-ArtId -Id $child -Ctx $Ctx }
    if (-not $Ctx.Result.Contains($entry.Dir)) { $Ctx.Result.Add($entry.Dir) }
  }

  foreach ($id in $RootRequired) { Visit-ArtId -Id $id -Ctx $ctx }

  return $result
}

function Get-ArtInfo {
  param([string]$ProjectDir)

  $artXmls = @(Get-ChildItem -LiteralPath $ProjectDir -Filter *.Art.xml -File -ErrorAction SilentlyContinue)
  if ($artXmls.Count -gt 1) {
    throw "There is more than one .Art.xml file in the project root; exactly one is allowed."
  }

  $hasArt = $artXmls.Count -eq 1
  $artXml = $null
  if ($hasArt) { $artXml = $artXmls[0].FullName }

  $artDefFiles = @()
  $artDefDir = Join-Path $ProjectDir 'ArtDefs'
  if (Test-Path -LiteralPath $artDefDir -PathType Container) {
    $artDefFiles = @(Get-ChildItem -LiteralPath $artDefDir -Filter *.artdef -File -ErrorAction SilentlyContinue | Sort-Object Name)
  }

  $xlpFiles = @()
  $xlpDir = Join-Path $ProjectDir 'XLPs'
  if (Test-Path -LiteralPath $xlpDir -PathType Container) {
    $xlpFiles = @(Get-ChildItem -LiteralPath $xlpDir -Filter *.xlp -File -ErrorAction SilentlyContinue | Sort-Object Name)
  }

  return [pscustomobject]@{ HasArt = $hasArt; ArtXml = $artXml; ArtDefs = $artDefFiles; Xlps = $xlpFiles }
}

function Assert-CookerReady {
  param($Ctx)

  $cookerDir = Join-Path (Join-Path $Ctx.BaseSdk 'AssetModTools') 'Cooker'
  $exe = Join-Path $cookerDir 'Civ6AssetCooker_FinalRelease.exe'
  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Cannot find the asset cooker at '$exe'. The mod includes art assets, so the cooker is required."
  }
  if (-not (Test-Path -LiteralPath $Ctx.BaseAssets -PathType Container)) {
    throw "Base assets path not found: $($Ctx.BaseAssets)"
  }
}

function Invoke-CookerCommand {
  param([string]$Exe, [string[]]$Argv, [string]$WorkingDirectory, [string]$Description)

  $code = 0
  Push-Location -LiteralPath $WorkingDirectory
  try {
    & $Exe @Argv | Out-Host
    $code = $LASTEXITCODE
  }
  catch {
    throw "Failed to launch the asset cooker for ${Description}: $($_.Exception.Message)"
  }
  finally {
    Pop-Location
  }

  if ($code -ne 0) {
    throw "The asset cooker failed for $Description (exit code $code)."
  }
}

function Invoke-Cooker {
  param($Ctx, $ArtInfo)

  $cookerDir = Join-Path (Join-Path $Ctx.BaseSdk 'AssetModTools') 'Cooker'
  $exe = Join-Path $cookerDir 'Civ6AssetCooker_FinalRelease.exe'
  $cfg = Join-Path $cookerDir 'Civ6.cfg'

  if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Cannot find the asset cooker at '$exe'. The mod includes art assets, so the cooker is required."
  }

  $rootInfo = Read-ArtXmlInfo $ArtInfo.ArtXml
  $pantries = @(Resolve-PantryPaths -ProjectDir $Ctx.ProjectDir -BaseAssets $Ctx.BaseAssets -RootRequired $rootInfo.Required)

  $baseArgs = @('--absolute_paths', '--no_mt')
  $pantryArgs = @('--pantry', $Ctx.ProjectDir) + $pantries
  $banquet = Join-Path $Ctx.Out 'ArtDefs'
  $stewpotWindows = Join-Path (Join-Path (Join-Path $Ctx.Out 'Platforms') 'Windows') 'BLPs'
  $stewpotMacOS = Join-Path (Join-Path (Join-Path $Ctx.Out 'Platforms') 'MacOS') 'BLPs'

  $cooks = 0

  foreach ($file in $ArtInfo.ArtDefs) {
    $argv = $baseArgs + @('--mode', 'ArtDef', '--platform', 'Windows', '--shaders', $cookerDir) + $pantryArgs + @('--banquet_hall', $banquet, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("ArtDef " + $file.Name)
    $cooks++
  }

  foreach ($file in $ArtInfo.Xlps) {
    $argv = $baseArgs + @('--mode', 'XLP', '--platform', 'Windows', '--shaders', $cookerDir) + $pantryArgs + @('--stewpot', $stewpotWindows, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("XLP " + $file.Name + " (Windows)")
    $cooks++

    $argv = $baseArgs + @('--mode', 'XLP', '--platform', 'MacOS', '--shaders', $cookerDir) + $pantryArgs + @('--stewpot', $stewpotMacOS, '--config', $cfg, $file.FullName)
    Invoke-CookerCommand -Exe $exe -Argv $argv -WorkingDirectory $Ctx.ProjectDir -Description ("XLP " + $file.Name + " (MacOS)")
    $cooks++
  }

  return $cooks
}

function Resolve-DepPath {
  param([string]$Out, [string]$ProjectDir)

  if (-not (Test-Path -LiteralPath $Out -PathType Container)) {
    throw "Output directory not found: $Out"
  }

  $deps = @(Get-ChildItem -LiteralPath $Out -Recurse -Filter *.dep -File -ErrorAction SilentlyContinue)

  if ($deps.Count -eq 0) {
    $strays = @(Get-ChildItem -LiteralPath $ProjectDir -Filter *.dep -File -ErrorAction SilentlyContinue)
    if ($strays.Count -eq 1) {
      $dest = Join-Path $Out $strays[0].Name
      Move-Item -LiteralPath $strays[0].FullName -Destination $dest -Force -ErrorAction Stop
      Write-Warning "The asset cooker wrote '$($strays[0].Name)' into the project directory; moved it into the output directory."
      $deps = @(Get-Item -LiteralPath $dest)
    }
  }

  if ($deps.Count -eq 0) {
    throw "No '.dep' art dependency file was found in '$Out'. Run a full build (without --skip-art) first."
  }
  if ($deps.Count -gt 1) {
    throw "More than one '.dep' file was found under '$Out'; cannot determine the art dependency file."
  }

  $rel = $deps[0].FullName.Substring($Out.Length).TrimStart('\', '/')
  return ($rel -replace '\\', '/')
}

function ConvertFrom-XmlFragment {
  param([string]$Xml)
  $doc = New-Object System.Xml.XmlDocument
  try {
    $doc.LoadXml($Xml)
  }
  catch {
    throw "Failed to parse project data fragment: $($_.Exception.Message)"
  }
  return $doc.DocumentElement
}

function Add-TextElement {
  param($Document, $Parent, [string]$Name, [string]$Value)
  $el = $Document.CreateElement($Name)
  $el.InnerText = $Value
  $Parent.AppendChild($el) | Out-Null
}

function New-ModInfo {
  param($Ctx, $Project, [string]$DepRelativePath, $CopiedFiles)

  $modDoc = New-Object System.Xml.XmlDocument
  $decl = $modDoc.CreateXmlDeclaration('1.0', 'utf-8', $null)
  $modDoc.AppendChild($decl) | Out-Null

  $root = $modDoc.CreateElement('Mod')
  $root.SetAttribute('id', $Project.Guid)
  $root.SetAttribute('version', $Project.ModVersion)
  $modDoc.AppendChild($root) | Out-Null

  $props = $modDoc.CreateElement('Properties')
  Add-TextElement -Document $modDoc -Parent $props -Name 'Name' -Value $Project.Name
  Add-TextElement -Document $modDoc -Parent $props -Name 'Description' -Value $Project.Description
  Add-TextElement -Document $modDoc -Parent $props -Name 'Created' -Value ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString())
  Add-TextElement -Document $modDoc -Parent $props -Name 'Teaser' -Value $Project.Teaser
  Add-TextElement -Document $modDoc -Parent $props -Name 'Authors' -Value $Project.Authors
  Add-TextElement -Document $modDoc -Parent $props -Name 'CompatibleVersions' -Value $Project.CompatibleVersions
  if (-not [string]::IsNullOrEmpty($Project.SpecialThanks)) {
    Add-TextElement -Document $modDoc -Parent $props -Name 'SpecialThanks' -Value $Project.SpecialThanks
  }
  if (-not [string]::IsNullOrEmpty($Project.Homepage)) {
    Add-TextElement -Document $modDoc -Parent $props -Name 'Homepage' -Value $Project.Homepage
  }
  $root.AppendChild($props) | Out-Null

  if (-not [string]::IsNullOrEmpty($Project.AssociationData)) {
    $assocDoc = New-Object System.Xml.XmlDocument
    $assocDoc.LoadXml($Project.AssociationData)
    $blocks = $modDoc.CreateElement('Blocks')
    foreach ($node in $assocDoc.DocumentElement.ChildNodes) {
      if ($node.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
      if ($node.Name -ne 'Block') { continue }
      $blockMod = $modDoc.CreateElement('Mod')
      $blockMod.SetAttribute('id', $node.GetAttribute('id'))
      $blockMod.SetAttribute('title', $node.GetAttribute('title'))
      $blocks.AppendChild($blockMod) | Out-Null
    }
    if ($blocks.HasChildNodes) { $root.AppendChild($blocks) | Out-Null }
  }

  if (-not [string]::IsNullOrEmpty($Project.ActionCriteriaData)) {
    $root.AppendChild($modDoc.ImportNode((ConvertFrom-XmlFragment $Project.ActionCriteriaData), $true)) | Out-Null
  }

  $frontEnd = $null
  if (-not [string]::IsNullOrEmpty($Project.FrontEndActionData)) {
    $frontEnd = $modDoc.ImportNode((ConvertFrom-XmlFragment $Project.FrontEndActionData), $true)
    $root.AppendChild($frontEnd) | Out-Null
  }

  $inGame = $null
  if (-not [string]::IsNullOrEmpty($Project.InGameActionData)) {
    $inGame = $modDoc.ImportNode((ConvertFrom-XmlFragment $Project.InGameActionData), $true)
    $root.AppendChild($inGame) | Out-Null
  }

  foreach ($container in @($frontEnd, $inGame)) {
    if ($null -eq $container) { continue }
    foreach ($file in $container.SelectNodes('.//File')) {
      if ($file.InnerText -eq '(Mod Art Dependency File)') {
        $file.InnerText = $DepRelativePath
      }
    }
  }

  if (-not [string]::IsNullOrEmpty($Project.LocalizedTextData)) {
    $root.AppendChild($modDoc.ImportNode((ConvertFrom-XmlFragment $Project.LocalizedTextData), $true)) | Out-Null
  }

  $modInfoPath = Join-Path $Ctx.Out ($Ctx.ProjectName + '.modinfo')

  $expected = New-Object System.Collections.Generic.List[string]
  foreach ($rel in $CopiedFiles) { $expected.Add($rel) }

  foreach ($f in @(Get-ChildItem -LiteralPath $Ctx.Out -File -Filter *.dep -ErrorAction SilentlyContinue)) {
    $expected.Add((($f.FullName.Substring($Ctx.Out.Length).TrimStart('\', '/')) -replace '\\', '/'))
  }

  foreach ($sub in @('ArtDefs', 'Platforms')) {
    $dir = Join-Path $Ctx.Out $sub
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue)) {
      $expected.Add((($f.FullName.Substring($Ctx.Out.Length).TrimStart('\', '/')) -replace '\\', '/'))
    }
  }

  $filesEl = $modDoc.CreateElement('Files')
  foreach ($rel in ($expected | Sort-Object -Unique)) {
    $fileEl = $modDoc.CreateElement('File')
    $fileEl.InnerText = $rel
    $filesEl.AppendChild($fileEl) | Out-Null
  }
  $root.AppendChild($filesEl) | Out-Null

  $settings = New-Object System.Xml.XmlWriterSettings
  $settings.Indent = $true
  $settings.IndentChars = '  '
  $settings.Encoding = New-Object System.Text.UTF8Encoding($true)

  $writer = [System.Xml.XmlWriter]::Create($modInfoPath, $settings)
  try {
    $modDoc.Save($writer)
  }
  finally {
    $writer.Dispose()
  }

  return $modInfoPath
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

  $project = Read-Project -Path $ctx.InputPath
  $artInfo = Get-ArtInfo -ProjectDir $ctx.ProjectDir
  $willCook = (-not $ctx.SkipArt) -and $artInfo.HasArt -and ($artInfo.ArtDefs.Count -gt 0 -or $artInfo.Xlps.Count -gt 0)
  if ($willCook) { Assert-CookerReady -Ctx $ctx }

  if ($ctx.Clean) {
    Invoke-Clean -Out $ctx.Out -ProjectDir $ctx.ProjectDir -DefaultOut $ctx.DefaultOut
  }

  $copyResult = Copy-ContentFiles -ContentList $project.Content -ProjectDir $ctx.ProjectDir -Out $ctx.Out -SkipArt $ctx.SkipArt

  $cooks = 0
  if ($willCook) {
    $cooks = Invoke-Cooker -Ctx $ctx -ArtInfo $artInfo
  }

  $depRel = Resolve-DepPath -Out $ctx.Out -ProjectDir $ctx.ProjectDir
  $modInfoPath = New-ModInfo -Ctx $ctx -Project $project -DepRelativePath $depRel -CopiedFiles $copyResult.Files

  Write-Host ("Copied {0} content file(s)." -f $copyResult.Count)
  Write-Host ("Ran {0} asset cook(s)." -f $cooks)
  Write-Host ("Wrote {0}" -f $modInfoPath)
  return 0
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

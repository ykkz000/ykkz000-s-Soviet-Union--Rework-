#Requires -Version 5.1

# Enables Windows Error Reporting LocalDumps for Civilization VI so that a full
# user-mode dump is written on crash. Run from an elevated PowerShell prompt:
#   powershell -ExecutionPolicy Bypass -File .tools\enable-local-dumps.ps1
#
# Without an elevated shell the HKLM writes fail with "Access is denied", and no
# dump is produced for the game process.

param(
  [string]$DumpFolder = 'C:\CrashDumps',
  [int]$DumpType = 2
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  [Console]::Error.WriteLine('This script must run from an elevated (Run as administrator) PowerShell.')
  exit 1
}

if (-not (Test-Path -LiteralPath $DumpFolder)) {
  New-Item -ItemType Directory -Path $DumpFolder -Force | Out-Null
}

$root = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'
$executables = @('CivilizationVI_DX12.exe', 'CivilizationVI.exe')

foreach ($exe in $executables) {
  $key = Join-Path $root $exe
  New-Item -Path $key -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpFolder' -PropertyType ExpandString -Value $DumpFolder -Force | Out-Null
  New-ItemProperty -Path $key -Name 'DumpType' -PropertyType DWord -Value $DumpType -Force | Out-Null
  Write-Host ("LocalDumps enabled for {0}: DumpFolder={1} DumpType={2}" -f $exe, $DumpFolder, $DumpType)
}

exit 0

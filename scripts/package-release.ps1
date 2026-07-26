# Package a portable Windows zip of Shays World VK.
# Usage (from modern/):
#   powershell -File scripts/package-release.ps1
#   powershell -File scripts/package-release.ps1 -BuildDir build -Config Release

param(
  [string]$BuildDir = "build",
  [string]$Config = "Release",
  [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $Root "$BuildDir\dist" }

$Exe = Join-Path $Root "$BuildDir\$Config\shays_vk.exe"
if (-not (Test-Path $Exe)) {
  $Exe = Join-Path $Root "$BuildDir\shays_vk.exe" # Ninja / single-config
}
if (-not (Test-Path $Exe)) {
  throw "shays_vk.exe not found under $BuildDir (build Release first)"
}

$Assets = Join-Path $Root "assets"
$Shaders = Join-Path $Root "$BuildDir\shaders"
if (-not (Test-Path (Join-Path $Shaders "textured.frag.spv"))) {
  $Beside = Join-Path (Split-Path $Exe) "shaders"
  if (Test-Path (Join-Path $Beside "textured.frag.spv")) { $Shaders = $Beside }
}
if (-not (Test-Path (Join-Path $Assets "scene.bin"))) {
  throw "assets/scene.bin missing"
}

Write-Host "Packaging:"
Write-Host "  exe     $Exe"
Write-Host "  assets  $Assets"
Write-Host "  shaders $Shaders"
Write-Host "  out     $OutDir"

& cmake `
  "-DSHAYS_EXE=$Exe" `
  "-DSHAYS_ASSETS=$Assets" `
  "-DSHAYS_SHADERS=$Shaders" `
  "-DSHAYS_OUT_DIR=$OutDir" `
  "-DSHAYS_README=$(Join-Path $Root 'README.md')" `
  -P (Join-Path $PSScriptRoot "package_release.cmake")

$Zip = Join-Path $OutDir "shays-world-vk-windows.zip"
Write-Host "Done: $Zip"

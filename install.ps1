# Ставит собранный winmm.dll в папку игры (игра должна быть закрыта).
param([string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\The Last of Us Part I")
$ErrorActionPreference = "Stop"
if (Get-Process tlou-i -ErrorAction SilentlyContinue) { throw "Сначала закрой игру." }
if (-not (Test-Path "$GameDir\tlou-i.exe")) { throw "tlou-i.exe не найден в $GameDir" }
Copy-Item "$PSScriptRoot\build\winmm.dll" "$GameDir\winmm.dll" -Force
Write-Host "Установлено: $GameDir\winmm.dll"

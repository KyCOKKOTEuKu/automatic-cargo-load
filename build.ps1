# Пересборка KOTEuKA_CargoLoad компилятором Visual Studio (C, x64).
# Запуск: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Не найден Visual Studio с компонентом C++ (MSVC x64).' }
$vc = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $PSScriptRoot 'src'
$out = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$flags = '/nologo /utf-8 /O2 /MT /LD /TC /W3 /D_CRT_SECURE_NO_WARNINGS /wd4244 /wd4101 /wd4305 /wd4028'
cmd /c "`"$vc`" >nul && cd /d `"$out`" && cl $flags `"$src\KOTEuKA_CargoLoad.c`" /Fe:KOTEuKA_CargoLoad.dll /link kernel32.lib user32.lib gdi32.lib && cl $flags `"$src\version_proxy.c`" /Fe:version.dll /link /DEF:`"$src\version.def`" kernel32.lib"
if ($LASTEXITCODE -ne 0) { throw 'Сборка не удалась.' }
Copy-Item "$out\KOTEuKA_CargoLoad.dll", "$out\version.dll", "$src\KOTEuKA_CargoLoad.ini", "$src\KOTEuKA_CargoLoad.profiles" $PSScriptRoot -Force
Write-Host 'Готово: файлы мода обновлены рядом с build.ps1'

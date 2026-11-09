param([string]$Compiler = 'tcc.exe')

$ErrorActionPreference = 'Stop'
$tcc = (Get-Command $Compiler -CommandType Application).Source
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Path build -Force | Out-Null

    & $tcc -shared -o build/RoadCraftAutoload.dll autoload.c -lkernel32 -luser32 -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'Failed to build RoadCraftAutoload.dll.' }

    & $tcc -shared -o build/version.dll version_proxy.c -lkernel32
    if ($LASTEXITCODE -ne 0) { throw 'Failed to build version.dll.' }

    Copy-Item -LiteralPath RoadCraftAutoload.profiles,RoadCraftAutoload.ini -Destination build
} finally {
    Pop-Location
}

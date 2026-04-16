# build_editor.ps1 — Build the FlareSim Lens Editor (Windows, VS2022 + CUDA)
#
# Usage:
#   .\build_editor.ps1              # Release build
#   .\build_editor.ps1 -Config Debug
#   .\build_editor.ps1 -Clean       # wipe build directory first

param(
    [string] $Config = "Release",
    [switch] $Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$editorDir = Join-Path $root "lens_editor"
$buildDir = Join-Path $editorDir "build"

Write-Host ""
Write-Host "FlareSim Lens Editor build  ($Config)" -ForegroundColor Cyan
Write-Host "Source : $editorDir"
Write-Host "Build  : $buildDir"
Write-Host ""

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

New-Item -ItemType Directory -Force $buildDir | Out-Null
Push-Location $buildDir

try {
    cmake "$editorDir" `
        -G "Visual Studio 17 2022" `
        -A x64

    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    cmake --build . --config $Config --parallel

    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    $exePath = Join-Path $editorDir "bin\$Config\FlareSim_LensEditor.exe"
    if (-not (Test-Path $exePath)) {
        # Flat bin/ layout (some generators drop the config sub-dir)
        $exePath = Join-Path $editorDir "bin\FlareSim_LensEditor.exe"
    }

    Write-Host ""
    if (Test-Path $exePath) {
        Write-Host "Build succeeded:" -ForegroundColor Green
        Write-Host "  $exePath"
    }
    else {
        Write-Host "Build finished (binary location may vary)." -ForegroundColor Yellow
    }
}
finally {
    Pop-Location
}

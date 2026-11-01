<#
.SYNOPSIS
  Build FlowerFinderBase (CLI executable and/or JNI bridge) with MSYS2 UCRT64 + Ninja.

.DESCRIPTION
  Artifacts land in <repo>\build\bin:
    FlowerFinderBase-cli.exe
    libFlowerFarmFinderLibJ.dll   (only when a JDK with include\jni.h is found)

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\build.ps1
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -CliOnly
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
#>
param(
    [switch]$CliOnly,
    [switch]$JniOnly,
    [switch]$Clean,
    [string]$BuildDir = 'build',
    [string]$Msys2Root = 'C:\msys64'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$ucrtBin = Join-Path $Msys2Root 'ucrt64\bin'
foreach ($tool in @('gcc.exe', 'g++.exe', 'cmake.exe', 'ninja.exe')) {
    if (-not (Test-Path (Join-Path $ucrtBin $tool))) {
        throw "MSYS2 UCRT64 tool '$tool' not found in $ucrtBin. Install MSYS2 and the mingw-w64-ucrt-x86_64-gcc / cmake / ninja packages."
    }
}
$env:PATH = "$ucrtBin;$env:PATH"

# Prefer a JDK that actually ships include\jni.h (JAVA_HOME is sometimes set to <jdk>\bin).
$jdkCandidates = @()
if ($env:JAVA_HOME) { $jdkCandidates += ($env:JAVA_HOME -replace '[\\/]+$', '' -replace '[\\/]bin$', '') }
$jdkCandidates += @('C:\Program Files\Java\jdk-17', 'C:\Program Files\Java\jdk-21', 'C:\Program Files\Java\latest')
$jdk = $jdkCandidates | Where-Object { $_ -and (Test-Path (Join-Path $_ 'include\jni.h')) } | Select-Object -First 1

if (-not (Test-Path (Join-Path $root 'cubiomes\generator.c'))) {
    Write-Warning "cubiomes submodule is empty. Run: git submodule update --init --recursive"
}

$buildPath = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $buildPath)) {
    Remove-Item -Recurse -Force $buildPath
}

$cmakeArgs = @('-S', $root, '-B', $buildPath, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
if ($jdk) { $cmakeArgs += "-DFFB_JAVA_HOME=$jdk" }

Write-Host "[FlowerFinderBase] cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

if ($CliOnly) {
    & cmake --build $buildPath --target FlowerFinderBase-cli
} elseif ($JniOnly) {
    & cmake --build $buildPath --target FlowerFinderBaseJni
} else {
    & cmake --build $buildPath
}
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$bin = Join-Path $buildPath 'bin'
Write-Host ""
Get-ChildItem $bin -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in @('.exe', '.dll') } |
    ForEach-Object { Write-Host ("  {0,-34} {1,10:N0} bytes" -f $_.Name, $_.Length) }
if (-not $jdk) {
    Write-Host "  (JNI bridge skipped: no JDK with include\jni.h found)"
}

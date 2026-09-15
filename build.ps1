param(
    [switch]$TestOnly,
    [switch]$Release
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDirectory = Join-Path $projectRoot 'build'
$temporaryDirectory = Join-Path $projectRoot '.tmp'
$mingwBin = 'D:\Database\MinGW\bin'
$gccLibexec = 'D:\Database\MinGW\libexec\gcc\mingw32\6.3.0'
$cmakeBin = 'D:\Database\CMake\bin'
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'

New-Item -ItemType Directory -Force -Path $temporaryDirectory | Out-Null

# The compiler's cc1plus.exe is under libexec, while its runtime DLLs are in
# the MinGW bin directory. Keep both locations in PATH for every child process
# and keep temporary files inside this D:-only project.
$env:PATH = "$mingwBin;$gccLibexec;$cmakeBin;$env:PATH"
$env:TEMP = $temporaryDirectory
$env:TMP = $temporaryDirectory

if (-not $TestOnly) {
    if ($Release) {
        & $cmake -S $projectRoot -B $buildDirectory -G 'MinGW Makefiles' `
            -DCMAKE_CXX_COMPILER='D:/Database/MinGW/bin/g++.exe' -DCMAKE_BUILD_TYPE=Release `
            '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -fomit-frame-pointer -DNDEBUG'
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if (-not (Test-Path (Join-Path $buildDirectory 'CMakeCache.txt'))) {
        & $cmake -S $projectRoot -B $buildDirectory -G 'MinGW Makefiles' `
            -DCMAKE_CXX_COMPILER='D:/Database/MinGW/bin/g++.exe' -DCMAKE_BUILD_TYPE=Release `
            '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -fomit-frame-pointer -DNDEBUG'
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    & $cmake --build $buildDirectory -- -j2
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $ctest --test-dir $buildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $TestOnly) {
    Copy-Item -LiteralPath (Join-Path $buildDirectory 'ngba.exe') -Destination (Join-Path $projectRoot 'ngba.exe') -Force
    Write-Host 'Ready: ngba.exe (double-click to select a ROM)'
}
exit $LASTEXITCODE

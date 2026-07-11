param(
    [string]$CMakePath = "C:\Program Files\CMake\bin\cmake.exe",
    [string]$InnoCompilerPath = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    [string]$PrefixPath = "C:/obs-dev/obs-prefix;C:/obs-dev/obs-studio-32.1.2/.deps/obs-deps-2025-08-23-x64;C:/obs-dev/obs-studio-32.1.2/.deps/obs-deps-qt6-2025-08-23-x64",
    [ValidateRange(0, 256)]
    [int]$Parallel = 0,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$version = (Get-Content -Raw (Join-Path $repoRoot "VERSION")).Trim()

$releaseRoot = Join-Path $repoRoot "release"
$installRoot = Join-Path $releaseRoot "windows-x64"
$installerRoot = Join-Path $releaseRoot "installer"
$packagesRoot = Join-Path $releaseRoot "packages"
$portableZip = Join-Path $packagesRoot "obs-preview-plugin.windows-x64-portable.zip"
$installerZip = Join-Path $packagesRoot "obs-preview-plugin.windows-x64-installer.zip"
$installerExe = Join-Path $installerRoot "obs-lan-preview-$version-windows-x64-installer.exe"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

if (-not (Test-Path $CMakePath)) {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) {
        throw "CMake was not found. Pass -CMakePath or add cmake to PATH."
    }
    $CMakePath = $cmakeCommand.Source
}

if (-not (Test-Path $InnoCompilerPath)) {
    $isccCommand = Get-Command iscc -ErrorAction SilentlyContinue
    if ($isccCommand) {
        $InnoCompilerPath = $isccCommand.Source
    } else {
        $candidates = @(
            "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
        )

        $InnoCompilerPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $InnoCompilerPath) {
            throw "ISCC.exe was not found. Pass -InnoCompilerPath."
        }
    }
}

if (-not $SkipBuild) {
    Invoke-Checked -FilePath $CMakePath -Arguments @("--preset", "windows-x64", "-DCMAKE_PREFIX_PATH=$PrefixPath")

    $buildArguments = @("--build", "--preset", "windows-x64-release", "--parallel")
    if ($Parallel -gt 0) {
        $buildArguments += $Parallel
    }
    Invoke-Checked -FilePath $CMakePath -Arguments $buildArguments

    Invoke-Checked -FilePath $CMakePath -Arguments @("--install", "build/windows-x64", "--config", "Release")
    Invoke-Checked -FilePath $InnoCompilerPath -Arguments @("installer/obs-lan-preview.iss")
}

if (-not (Test-Path (Join-Path $installRoot "obs-plugins\64bit\obs-lan-preview.dll"))) {
    throw "Portable plugin DLL not found. Build/install first."
}

if (-not (Test-Path $installerExe)) {
    throw "Installer executable not found: $installerExe"
}

New-Item -ItemType Directory -Force -Path $packagesRoot | Out-Null
Remove-Item -LiteralPath $portableZip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installerZip -Force -ErrorAction SilentlyContinue

Compress-Archive -Path (Join-Path $installRoot "*") -DestinationPath $portableZip -Force
Compress-Archive -Path $installerExe -DestinationPath $installerZip -Force

Write-Host "Packaged:" -ForegroundColor Green
Write-Host "  $portableZip"
Write-Host "  $installerZip"

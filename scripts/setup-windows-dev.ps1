param(
    [string]$DevRoot = "C:\obs-dev",
    [string]$ObsDepsDate = "2025-08-23",
    [string]$ObsVersion = "32.1.2",
    [switch]$SkipDownloads
)

$ErrorActionPreference = "Stop"

function Install-WingetPackage {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [string]$Override
    )

    $installed = winget list --id $Id --exact --accept-source-agreements 2>$null
    if ($LASTEXITCODE -eq 0 -and ($installed -match [regex]::Escape($Id))) {
        Write-Host "$Id is already installed. Skipping winget install." -ForegroundColor DarkGray
        return
    }

    Write-Host "Installing $Id..." -ForegroundColor Cyan

    $args = @(
        "install",
        "--id", $Id,
        "--exact",
        "--accept-package-agreements",
        "--accept-source-agreements",
        "--silent"
    )

    if ($Override) {
        $args += @("--override", $Override)
    }

    winget @args
}

function Download-And-Expand {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $fileName = Split-Path $Url -Leaf
    $downloadPath = Join-Path $DevRoot $fileName
    $sentinelPath = Join-Path $Destination ".obs-lan-preview-extracted"

    if ((Test-Path $Destination) -and (Test-Path $sentinelPath)) {
        Write-Host "Using existing extracted package $Destination" -ForegroundColor DarkGray
        return
    }

    if (-not (Test-Path $downloadPath)) {
        Write-Host "Downloading $fileName..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $Url -OutFile $downloadPath
    } else {
        Write-Host "Using existing $downloadPath"
    }

    if (-not (Test-Path $Destination)) {
        Write-Host "Extracting to $Destination..." -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
        Expand-Archive -Path $downloadPath -DestinationPath $Destination -Force
    } else {
        Write-Host "Refreshing extraction in $Destination..." -ForegroundColor Cyan
        Expand-Archive -Path $downloadPath -DestinationPath $Destination -Force
    }

    Set-Content -Path $sentinelPath -Value "Extracted from $fileName on $(Get-Date -Format o)"
}

function Get-IsccPath {
    $command = Get-Command iscc -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

function Clone-ObsStudio {
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Version
    )

    if (Test-Path $Destination) {
        if (Test-Path (Join-Path $Destination ".git")) {
            Write-Host "Using existing OBS Studio git clone $Destination" -ForegroundColor DarkGray
            return
        }

        Write-Host "$Destination exists but is not a git clone. Leaving it untouched." -ForegroundColor Yellow
        Write-Host "Use a different -DevRoot or remove/rename that directory if you want the script to clone OBS there." -ForegroundColor Yellow
        return
    }

    Write-Host "Cloning OBS Studio $Version with submodules..." -ForegroundColor Cyan
    git clone --recursive --branch $Version https://github.com/obsproject/obs-studio.git $Destination
}

function Get-VsWherePath {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

function Test-VsBuildTools {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $false
    }

    $installationPath = & $vswhere -latest -products Microsoft.VisualStudio.Product.BuildTools -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
    return -not [string]::IsNullOrWhiteSpace($installationPath)
}

function Get-VsBuildToolsPath {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $null
    }

    $installationPath = & $vswhere -latest -products Microsoft.VisualStudio.Product.BuildTools -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
    if ([string]::IsNullOrWhiteSpace($installationPath)) {
        return $null
    }

    return $installationPath.Trim()
}

function Install-VsBuildToolsComponents {
    $installPath = Get-VsBuildToolsPath
    $installerPath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vs_installer.exe"

    if ($installPath -and (Test-Path $installerPath)) {
        Write-Host "Adding missing Visual Studio Build Tools components..." -ForegroundColor Cyan
        & $installerPath modify `
            --installPath $installPath `
            --quiet `
            --wait `
            --norestart `
            --add Microsoft.VisualStudio.Workload.VCTools `
            --includeRecommended `
            --add Microsoft.VisualStudio.Component.Windows11SDK.22621
        return
    }

    Install-WingetPackage `
        -Id "Microsoft.VisualStudio.2022.BuildTools" `
        -Override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is not available. Install Windows App Installer from Microsoft Store first."
}

New-Item -ItemType Directory -Force -Path $DevRoot | Out-Null

Write-Host "Checking local build tools..." -ForegroundColor Cyan
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    Write-Host "cmake is already available: $((Get-Command cmake).Source)" -ForegroundColor DarkGray
} else {
    Install-WingetPackage -Id "Kitware.CMake"
}

if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "git is already available: $((Get-Command git).Source)" -ForegroundColor DarkGray
} else {
    Install-WingetPackage -Id "Git.Git"
}

if ($existingIscc = Get-IsccPath) {
    Write-Host "Inno Setup compiler is already available: $existingIscc" -ForegroundColor DarkGray
} else {
    Install-WingetPackage -Id "JRSoftware.InnoSetup"
}

if (Test-VsBuildTools) {
    Write-Host "Visual Studio Build Tools with VC Tools are already installed. Skipping install." -ForegroundColor DarkGray
} else {
    Install-VsBuildToolsComponents
}

if (-not $SkipDownloads) {
    $depsBaseUrl = "https://github.com/obsproject/obs-deps/releases/download/$ObsDepsDate"
    Download-And-Expand `
        -Url "$depsBaseUrl/windows-deps-$ObsDepsDate-x64.zip" `
        -Destination (Join-Path $DevRoot "windows-deps-$ObsDepsDate-x64")

    Download-And-Expand `
        -Url "$depsBaseUrl/windows-deps-qt6-$ObsDepsDate-x64.zip" `
        -Destination (Join-Path $DevRoot "windows-deps-qt6-$ObsDepsDate-x64")

    Clone-ObsStudio `
        -Destination (Join-Path $DevRoot "obs-studio-$ObsVersion") `
        -Version $ObsVersion
}

$isccPath = Get-IsccPath

Write-Host ""
Write-Host "Installed/free tools requested by this plugin:" -ForegroundColor Green
Write-Host "  - MSVC Build Tools 2022, no paid Visual Studio IDE required"
Write-Host "  - CMake"
Write-Host "  - Git"
Write-Host "  - Inno Setup"
if ($isccPath) {
    Write-Host "    ISCC: $isccPath"
} else {
    Write-Host "    ISCC was not found in PATH or the default Inno Setup directory." -ForegroundColor Yellow
}
Write-Host ""
Write-Host "OBS dependency archives were downloaded to: $DevRoot" -ForegroundColor Green
Write-Host ""
Write-Host "Next required piece:" -ForegroundColor Yellow
Write-Host "  This plugin still needs a libobs/obs-frontend-api development CMake package."
Write-Host "  The normal OBS Studio app installation usually does NOT include that package."
Write-Host "  The reliable route is to build/install OBS Studio once from source, then pass that install prefix plus the OBS deps/Qt deps to this plugin's CMake configure step."
Write-Host ""
Write-Host "After restarting the terminal, verify:"
Write-Host "  cmake --version"
if ($isccPath) {
    Write-Host "  & `"$isccPath`" /?"
} else {
    Write-Host "  iscc /?"
}
Write-Host "  Open 'x64 Native Tools Command Prompt for VS 2022' and run: cl"

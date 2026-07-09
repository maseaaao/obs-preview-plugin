# OBS LAN Preview Plugin

[![Release](https://github.com/maseaaao/obs-preview-plugin/actions/workflows/release.yml/badge.svg)](https://github.com/maseaaao/obs-preview-plugin/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/maseaaao/obs-preview-plugin?sort=semver)](https://github.com/maseaaao/obs-preview-plugin/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078d4.svg)](#build)

Windows-first OBS Studio plugin that serves the current Program output to the local network as a live MJPEG preview.

Default preview URL:

```text
http://<pc-lan-ip>:9181/
```

## Highlights

- Live Program preview in any browser on the same local network.
- Simple MJPEG stream with snapshot and health endpoints.
- OBS menu entry at `Tools -> LAN Preview`.
- Windows x64 installer and portable release packages.
- No external service, account, or cloud dependency.

## Demo

<video width="640" height="260" controls>
  <source src="docs/demo.mp4" type="video/mp4">
</video>

## Install

Download the latest Windows assets from the [Releases](https://github.com/maseaaao/obs-preview-plugin/releases) page.

Use one of these packages:

- `obs-preview-plugin.windows-x64-installer.zip` - installer build.
- `obs-preview-plugin.windows-x64-portable.zip` - portable OBS folder layout.

For the portable package, extract the archive into the OBS Studio installation directory so `obs-plugins\64bit\obs-lan-preview.dll` lands under the OBS root.

## Use

1. Install the plugin into OBS.
2. Open OBS.
3. Go to `Tools -> LAN Preview`.
4. Enable preview and apply settings.
5. Open the shown URL from another device on the same LAN.

Windows Firewall may ask whether OBS can accept local network connections. Allow private networks if you want phone or tablet access.

## Default Settings

- 5 FPS
- 640x360
- JPEG quality 70%
- Bind address `0.0.0.0`
- No password in v1

## HTTP Endpoints

- `/` - minimal browser page with the live preview
- `/preview.mjpg` - MJPEG stream
- `/snapshot.jpg` - latest JPEG frame
- `/health` - JSON status

## Security

v1 intentionally has no password. Anyone on the same network can view the preview while it is enabled.

Use it only on trusted private networks. See [SECURITY.md](SECURITY.md) for reporting guidance.

## Build

Requirements:

- Windows 10/11
- OBS Studio 32.x development files available to CMake
- Visual Studio 2022
- CMake 3.28+
- Qt6 matching the OBS build environment
- Inno Setup 6

Install the free command-line tooling with:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup-windows-dev.ps1
```

Configure and build:

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --preset windows-x64 `
  "-DCMAKE_PREFIX_PATH=C:/obs-dev/obs-prefix;C:/obs-dev/obs-studio-32.1.2/.deps/obs-deps-2025-08-23-x64;C:/obs-dev/obs-studio-32.1.2/.deps/obs-deps-qt6-2025-08-23-x64"

cmake --build --preset windows-x64-release
cmake --install build/windows-x64 --config Release
```

The plugin binary is `obs-lan-preview.dll`.

Build both release zip assets:

```powershell
.\scripts\package-windows.ps1
```

This creates:

```text
release\packages\obs-preview-plugin.windows-x64-installer.zip
release\packages\obs-preview-plugin.windows-x64-portable.zip
```

## OBS Development Prefix

The regular OBS Studio installation usually does not include `libobsConfig.cmake`.
Build and install only the development pieces from OBS source:

```powershell
cd C:\obs-dev\obs-studio-32.1.2

"C:\Program Files\CMake\bin\cmake.exe" --preset windows-x64 `
  -DCMAKE_INSTALL_PREFIX=C:/obs-dev/obs-prefix

"C:\Program Files\CMake\bin\cmake.exe" --build --preset windows-x64 --config RelWithDebInfo --target libobs obs-frontend-api
"C:\Program Files\CMake\bin\cmake.exe" --install build_x64 --prefix C:\obs-dev\obs-prefix --config RelWithDebInfo --component Development
```

This avoids building the full OBS application and plugins.

## Versioning and Releases

Use SemVer bumps:

```powershell
.\scripts\bump-version.ps1 patch
.\scripts\bump-version.ps1 minor
.\scripts\bump-version.ps1 major
```

The bump script updates `VERSION`, `CMakeLists.txt`, and the Inno Setup script, then creates a commit and tag like `v0.1.1`.

Push the tag to trigger GitHub Releases:

```powershell
git push
git push --tags
```

The GitHub Actions release workflow uploads:

```text
obs-preview-plugin.windows-x64-installer.zip
obs-preview-plugin.windows-x64-portable.zip
```

For manual publishing with GitHub CLI:

```powershell
.\scripts\package-windows.ps1
.\scripts\publish-github-release.ps1
```

## Contributing

Issues and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md), [SUPPORT.md](SUPPORT.md), and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

Distributed under the [MIT License](LICENSE).

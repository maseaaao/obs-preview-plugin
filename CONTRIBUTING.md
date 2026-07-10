# Contributing

Thanks for taking the time to improve LAN Preview.

## Good First Steps

1. Open an issue before large behavior changes.
2. Keep pull requests focused on one fix or feature.
3. Include screenshots or logs for UI, installer, and build changes when useful.
4. Update `README.md` or `CHANGELOG.md` when the user-facing behavior changes.

## Local Build

Install the Windows development environment:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup-windows-dev.ps1
```

Build and package:

```powershell
.\scripts\package-windows.ps1
```

The release assets are written to `release\packages`.

## Pull Request Checklist

- The project builds locally or the limitation is described in the PR.
- New behavior is documented.
- The change does not commit generated build output from `build/` or `release/`.
- The PR keeps the plugin Windows x64 release path working.

## Versioning

Use the version bump script for releases:

```powershell
.\scripts\bump-version.ps1 patch
```

The script updates version files, creates a commit, and creates a tag.

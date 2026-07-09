# Changelog

All notable changes to this project are documented here.

This project follows Semantic Versioning where practical.

## [Unreleased]

- Reduce idle and multi-client overhead by starting OBS raw frame capture only while frames are demanded, handling short health/index requests without per-request threads, sharing encoded JPEG frames across clients, adding frame backpressure, reusing raw frame buffers, capturing BGR frames directly, and reusing the WIC imaging factory.
- Add performance measurement documentation and local OBS process sampling script.
- Add repository documentation, community files, and MIT license.

## [0.1.4] - 2026-07-10

- Fix the Windows release workflow runner and add manual release asset publishing for existing tags.

## [0.1.3] - 2026-07-10

- Add Windows release workflow packaging installer and portable zip assets.
- Add scripts for local Windows development setup, packaging, release publishing, and version bumps.

## [0.1.0] - 2026-07-10

- Initial Windows OBS Studio plugin implementation.
- Add LAN MJPEG preview server with browser preview, snapshot, and health endpoints.
- Add OBS tools menu integration and settings dialog.

[Unreleased]: https://github.com/maseaaao/obs-preview-plugin/compare/v0.1.4...HEAD
[0.1.4]: https://github.com/maseaaao/obs-preview-plugin/releases/tag/v0.1.4
[0.1.3]: https://github.com/maseaaao/obs-preview-plugin/releases/tag/v0.1.3
[0.1.0]: https://github.com/maseaaao/obs-preview-plugin/releases/tag/v0.1.0

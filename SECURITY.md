# Security Policy

## Supported Versions

Security fixes target the latest released version of LAN Preview.

## Reporting a Vulnerability

Please do not publish exploit details in a public issue.

Report security concerns privately through GitHub's private vulnerability reporting if it is available for this repository. If that is not available, open a minimal public issue that says you have a security report and avoid sharing sensitive details there.

Useful details:

- Plugin version and OBS Studio version
- Windows version
- Whether preview authentication was enabled, if available in your build
- Network exposure details, such as LAN-only or routed/VPN access
- Steps to reproduce

## Network Security Notes

The current v1 preview is intended for trusted local networks. Anyone who can reach the preview endpoint can view the stream while the preview is enabled.

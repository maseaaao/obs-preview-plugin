# HTTPS, PWA, and Mobile Validation

Run these checks against a release build on a trusted LAN after enabling the plugin.

## TLS and certificate

1. Export the trusted-device CA from the LAN Preview settings.
2. Install and explicitly trust it on the test device.
3. On the OBS PC, validate the same certificate and URL:

```powershell
.\scripts\verify-https-preview.ps1 `
  -PreviewUrl "https://192.168.1.10:9181/" `
  -CaCertificate "C:\path\obs-lan-preview-ca.pem"
```

4. Open the URL on the mobile device. It must not show a certificate warning.

## Browser behavior

Test current Chrome on Android and Safari on iOS/iPadOS:

1. Open the normal URL and confirm live preview.
2. Open the stay-awake URL. With a short screen timeout, confirm the screen remains on while preview is visible.
3. Switch to another app or background tab; `/health` must soon show `streamClients: 0` and `frameDemand: false`.
4. Return to the preview; it must reconnect and resume demand.
5. Put the preview and another app in split screen. The preview remains visible and must continue streaming.
6. Add the page to Home Screen/install it. The launch icon must open a standalone preview; repeat steps 2-5.

## Performance

For 1, 25, and 35 FPS at 10%, 33%, and 100% scale, run `scripts/measure-obs-performance.ps1` with at least one visible client and again with no clients. Confirm that no-client CPU use returns to the idle baseline and that `/health` reports no active frame demand.

# Wi-Fi AP Onboarding Guide

This guide documents the Wi-Fi onboarding flow for firmware builds that include the MimiClaw onboarding portal.

## What It Does

When onboarding is enabled, MimiClaw can expose a local Wi-Fi access point such as `MimiClaw-XXXX` and serve a configuration page on the AP gateway address printed in the serial log (normally `http://192.168.50.1`).

Typical uses:

- first-time Wi-Fi setup without editing firmware
- updating saved Wi-Fi, model, proxy, or bot credentials later
- recovering a device that can no longer join the previous router

## Requirements

- a firmware build with the Wi-Fi onboarding portal enabled
- an ESP32-S3 board powered on and booted normally
- a phone or laptop that can join the temporary `MimiClaw-XXXX` network

## First-Time Setup

1. Power on the device.
2. Wait for the onboarding hotspot `MimiClaw-XXXX` to appear.
3. Join that hotspot from your phone or laptop.
4. Open the gateway address printed in the serial log if the captive page does not open automatically.
5. Fill in at least:
   - `SSID`
   - `Password`
6. Add optional settings if needed:
   - `API Key`
   - `Model`
   - `Provider`
   - `Telegram`
   - `Feishu`
   - `Proxy`
   - `Search`
7. Click `Save & Restart`.
8. Wait for the device to reboot and join your normal Wi-Fi network.

## Updating Settings Later

1. Hold the BOOT button for 3 seconds.
2. Join the temporary `MimiClaw-XXXX` network from a phone or laptop.
3. Open the gateway address printed in the serial log (normally `http://192.168.50.1`).
4. Make and save the required changes.

The admin hotspot and configuration server close automatically after 10 minutes. The normal STA connection and network services remain active. The page prefills the currently effective configuration so you can edit only the fields you want to change.

## Config Priority

The onboarding page follows the same config priority as the firmware:

1. saved NVS values
2. build-time defaults from `main/mimi_secrets.h`

That means:

- if a field exists in NVS, it overrides the build-time default
- if you clear a saved field and reboot, the device falls back to `main/mimi_secrets.h`
- if both NVS and build-time values are empty, the field stays empty

## Clearing Saved Values

On firmware versions that support clearing fields from the portal:

- leaving a field blank and saving removes the corresponding NVS key
- after reboot, the page shows the build-time fallback if one exists

If you need to wipe all saved runtime settings, use:

```text
mimi> config_reset
```

## Troubleshooting

### No `MimiClaw-XXXX` hotspot appears

- verify that the running firmware actually includes the onboarding portal
- if the device is already connected, hold BOOT for 3 seconds to open the hotspot
- if the device cannot connect and automatic onboarding does not start, clear Wi-Fi config and reboot
- confirm the board has finished booting before scanning for Wi-Fi

### The page still shows old values

- refresh the page manually
- reconnect to the AP and open `http://192.168.4.1` again
- if needed, restart the browser once to clear stale captive portal state

### The build-time provider/model do not show up

If the device already has `model` or `provider` saved in NVS, the saved values win over `main/mimi_secrets.h`.

To return to build-time defaults:

- clear those fields in the portal and save, or
- run `config_reset`

## Notes

- The onboarding AP is typically local-only and intended for nearby configuration.
- The AP is open for compatibility, so the normal admin flow limits it to a 10-minute physical-presence window.
- If your deployment needs stronger local protection, add an AP password before using the flow in production.

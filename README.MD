# Presence Indicator ("On Air" for Microsoft Teams)

This project is a physical "On Air" signal for Microsoft Teams.

An ESP32-CAM device polls Microsoft Graph presence for a selected user and drives the on-board LED as a busy indicator. The current behavior is:

- LED ON (dimmed via PWM) when activity is `InACall`, `InAConferenceCall`, or `DoNotDisturb`
- LED OFF for other states or when presence cannot be fetched

## What It Uses

- Visual Studio Code
- PlatformIO extension for VS Code
- PlatformIO Core/toolchain (installed via the extension)
- Board target: `esp32cam` (Arduino framework)
- Library dependency: `bblanchon/ArduinoJson`

## Entra ID (Azure AD) Setup

Create and configure an app registration that can read Microsoft Teams presence through Microsoft Graph.

1. In Microsoft Entra admin center, create a new App registration.
2. In API permissions, add Microsoft Graph permission `Presence.Read.All` (you may also see this written as `Presence.ReadAll`).
3. Grant admin consent for the tenant for that permission.
4. In Authentication, enable `Allow public client flows`.
5. In Certificates & secrets, create a client secret and save it.

You will need these values for firmware config:

- Tenant ID
- Application (client) ID
- Client secret value
- User to monitor (`presenceUserId`, for example UPN like `user@contoso.com`)

## Project Configuration

The firmware reads private settings from `include/private_config.h`.

1. Copy `include/private_config.example.h` to `include/private_config.h`.
2. Fill in:
	- `ssid`, `password`
	- `tenantId`, `clientId`, `clientSecret`
	- `presenceUserId`
3. Optionally adjust `presenceLedPin` if your board wiring differs.

Default scope in the example is:

- `https://graph.microsoft.com/.default`

## Build, Upload, Monitor

From the workspace root:

```bash
pio run
pio run --target upload
pio device monitor --baud 115200
```

Or use PlatformIO actions from VS Code (Build / Upload / Monitor).

## Runtime Notes

- Presence is polled periodically (30s normal interval, shorter retry interval on error).
- Access tokens are refreshed before expiry.
- If `presenceUserId` is a UPN/email, the firmware resolves it to object ID first.
- HTTPS uses insecure TLS mode on-device (`setInsecure()`), suitable for simple prototypes but not ideal for production-hardening.

## Security Notes

- `include/private_config.h` contains secrets. Keep it out of source control.
- Do not share logs that may include sensitive tenant/user details.

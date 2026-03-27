#pragma once

const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

const char *tenantId = "YOUR_TENANT_ID";
const char *clientId = "YOUR_CLIENT_ID";
const char *clientSecret = "YOUR_CLIENT_SECRET";

const char *tokenScope = "https://graph.microsoft.com/.default";
const char *presenceUserId = "user@contoso.com";

// AI Thinker ESP32-CAM flash LED is commonly on GPIO 4. Adjust if your board differs.
const uint8_t presenceLedPin = 4;

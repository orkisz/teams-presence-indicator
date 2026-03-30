#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "private_config.h"

const unsigned long presencePollIntervalMs = 30000UL;
const unsigned long presenceRetryIntervalMs = 5000UL;

// LEDC (hardware PWM) config for dimming the flash LED.
// Duty cycle is 8-bit (0–255). 10 ≈ 4% brightness which is
// visually dim but still clearly visible in a dark room.
constexpr uint8_t  ledPwmChannel   = 0;
constexpr uint32_t ledPwmFreq      = 1000;   // Hz, well above flicker threshold
constexpr uint8_t  ledPwmResolution = 8;     // bits
constexpr uint8_t  ledDimDutyCycle  = 10;    // 0–255; increase to make brighter

String currentAccessToken;
String resolvedPresenceUserObjectId;
unsigned long tokenRefreshAtMs = 0;
unsigned long nextPresenceQueryAtMs = 0;

void setPresenceLed(bool enabled)
{
  ledcWrite(ledPwmChannel, enabled ? ledDimDutyCycle : 0);
}

bool isUserInCall(const char *activity)
{
  if (activity == nullptr)
  {
    return false;
  }

  return strcmp(activity, "InACall") == 0 ||
         strcmp(activity, "InAConferenceCall") == 0 ||
         strcmp(activity, "DoNotDisturb") == 0 ||
         strcmp(activity, "Presenting") == 0;
}

void dumpJson(const char *label, const String &json)
{
  Serial.printf("%s JSON dump begin\n", label);
  Serial.println(json);
  Serial.printf("%s JSON dump end\n", label);
}

String urlEncode(const String &input)
{
  String encoded;
  const char *hex = "0123456789ABCDEF";

  for (size_t i = 0; i < input.length(); i++)
  {
    const char c = input.charAt(i);
    const bool isAlphaNum = (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9');
    const bool isSafe = c == '-' || c == '_' || c == '.' || c == '~';

    if (isAlphaNum || isSafe)
    {
      encoded += c;
    }
    else
    {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

bool isGuid(const String &value)
{
  if (value.length() != 36)
  {
    return false;
  }

  for (size_t i = 0; i < value.length(); i++)
  {
    const char c = value.charAt(i);
    const bool isHex = (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F') ||
                       (c >= '0' && c <= '9');
    const bool isDashPosition = i == 8 || i == 13 || i == 18 || i == 23;

    if (isDashPosition)
    {
      if (c != '-')
      {
        return false;
      }
    }
    else if (!isHex)
    {
      return false;
    }
  }

  return true;
}

bool resolvePresenceUserObjectId()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected. Cannot resolve Graph user.");
    return false;
  }

  if (currentAccessToken.isEmpty())
  {
    Serial.println("No access token available. Cannot resolve Graph user.");
    return false;
  }

  const String configuredUser = String(presenceUserId);
  if (isGuid(configuredUser))
  {
    resolvedPresenceUserObjectId = configuredUser;
    return true;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  const String userUrl =
      "https://graph.microsoft.com/v1.0/users/" + urlEncode(configuredUser) + "?$select=id,userPrincipalName";

  if (!http.begin(secureClient, userUrl))
  {
    Serial.println("Failed to initialize HTTP client for user lookup.");
    return false;
  }

  http.addHeader("Authorization", "Bearer " + currentAccessToken);
  http.addHeader("Accept", "application/json");

  const int statusCode = http.GET();
  const String responseBody = http.getString();
  http.end();

  if (statusCode <= 0)
  {
    Serial.printf("User lookup failed at transport level: %d\n", statusCode);
    return false;
  }

  if (statusCode != 200)
  {
    Serial.printf("User lookup failed. HTTP %d\n", statusCode);
    dumpJson("User lookup error", responseBody);
    return false;
  }

  dumpJson("User lookup", responseBody);

  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, responseBody);
  if (jsonError)
  {
    Serial.printf("Failed to parse user lookup JSON: %s\n", jsonError.c_str());
    return false;
  }

  const char *resolvedId = doc["id"];
  if (resolvedId == nullptr || strlen(resolvedId) == 0)
  {
    Serial.println("User lookup response does not include id.");
    return false;
  }

  resolvedPresenceUserObjectId = String(resolvedId);
  Serial.printf("Resolved %s to object ID %s\n", presenceUserId, resolvedPresenceUserObjectId.c_str());
  return true;
}

bool fetchAccessToken()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected. Cannot request Entra token.");
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  const String tokenUrl = "https://login.microsoftonline.com/" + String(tenantId) + "/oauth2/v2.0/token";

  if (!http.begin(secureClient, tokenUrl))
  {
    Serial.println("Failed to initialize HTTP client for token request.");
    return false;
  }

  const String requestBody =
      "client_id=" + urlEncode(String(clientId)) +
      "&client_secret=" + urlEncode(String(clientSecret)) +
      "&grant_type=client_credentials" +
      "&scope=" + urlEncode(String(tokenScope));

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int statusCode = http.POST(requestBody);
  const String responseBody = http.getString();
  http.end();

  if (statusCode <= 0)
  {
    Serial.printf("Token request failed at transport level: %d\n", statusCode);
    return false;
  }

  if (statusCode != 200)
  {
    Serial.printf("Token request failed. HTTP %d\n", statusCode);
    dumpJson("Token error", responseBody);
    Serial.println(responseBody);
    return false;
  }

  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, responseBody);
  if (jsonError)
  {
    Serial.printf("Failed to parse token JSON: %s\n", jsonError.c_str());
    return false;
  }

  const char *token = doc["access_token"];
  const uint32_t expiresIn = doc["expires_in"] | 3600;
  if (token == nullptr || strlen(token) == 0)
  {
    Serial.println("Token response does not include access_token.");
    return false;
  }

  currentAccessToken = String(token);
  resolvedPresenceUserObjectId = "";

  // Refresh 60 seconds before expiry, with underflow protection.
  const uint32_t refreshInSeconds = expiresIn > 60 ? (expiresIn - 60) : 30;
  tokenRefreshAtMs = millis() + (refreshInSeconds * 1000UL);

  Serial.println("Successfully fetched Entra access token.");
  Serial.printf("Token length: %u\n", static_cast<unsigned>(currentAccessToken.length()));
  Serial.printf("Next refresh in %lu seconds\n", static_cast<unsigned long>(refreshInSeconds));

  return true;
}

bool fetchUserPresence()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected. Cannot query Graph presence.");
    return false;
  }

  if (currentAccessToken.isEmpty())
  {
    Serial.println("No access token available. Cannot query Graph presence.");
    return false;
  }

  if (resolvedPresenceUserObjectId.isEmpty() && !resolvePresenceUserObjectId())
  {
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  const String presenceUrl =
      "https://graph.microsoft.com/v1.0/communications/presences/" + resolvedPresenceUserObjectId;

  if (!http.begin(secureClient, presenceUrl))
  {
    Serial.println("Failed to initialize HTTP client for presence request.");
    return false;
  }

  http.addHeader("Authorization", "Bearer " + currentAccessToken);
  http.addHeader("Accept", "application/json");

  const int statusCode = http.GET();
  const String responseBody = http.getString();
  http.end();

  if (statusCode <= 0)
  {
    Serial.printf("Presence request failed at transport level: %d\n", statusCode);
    return false;
  }

  if (statusCode != 200)
  {
    Serial.printf("Presence request failed. HTTP %d\n", statusCode);
    dumpJson("Presence error", responseBody);
    setPresenceLed(false);
    return false;
  }

  dumpJson("Presence", responseBody);

  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, responseBody);
  if (jsonError)
  {
    Serial.printf("Failed to parse presence JSON: %s\n", jsonError.c_str());
    setPresenceLed(false);
    return false;
  }

  JsonVariant presenceNode = doc["value"][0];
  if (presenceNode.isNull())
  {
    presenceNode = doc.as<JsonVariant>();
  }

  const char *availability = presenceNode["availability"] | "unknown";
  const char *activity = presenceNode["activity"] | "unknown";
  const char *outOfOffice = presenceNode["outOfOfficeSettings"]["isOutOfOffice"] ? "true" : "false";

  Serial.printf("Presence for %s (%s)\n", presenceUserId, resolvedPresenceUserObjectId.c_str());
  Serial.printf("  availability: %s\n", availability);
  Serial.printf("  activity: %s\n", activity);
  Serial.printf("  outOfOffice: %s\n", outOfOffice);

  setPresenceLed(isUserInCall(activity));

  return true;
}

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("Connected to AP successfully!");
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  Serial.println("Disconnected from WiFi access point");
  Serial.print("WiFi lost connection. Reason: ");
  Serial.println(info.wifi_sta_disconnected.reason);
  Serial.println("Trying to Reconnect");
  setPresenceLed(false);
  WiFi.begin(ssid, password);
}

void setup()
{
  Serial.begin(115200);
  ledcSetup(ledPwmChannel, ledPwmFreq, ledPwmResolution);
  ledcAttachPin(presenceLedPin, ledPwmChannel);
  setPresenceLed(false);

  // delete old config
  WiFi.disconnect(true);

  delay(1000);

  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  /* Remove WiFi event
  Serial.print("WiFi Event ID: ");
  Serial.println(eventID);
  WiFi.removeEvent(eventID);*/

  WiFi.begin(ssid, password);

  Serial.println();
  Serial.println();
  Serial.println("Wait for WiFi... ");
}

void loop()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (currentAccessToken.isEmpty() || millis() >= tokenRefreshAtMs)
    {
      if (fetchAccessToken())
      {
        nextPresenceQueryAtMs = 0;
      }
    }

    if (!currentAccessToken.isEmpty() && millis() >= nextPresenceQueryAtMs)
    {
      if (fetchUserPresence())
      {
        nextPresenceQueryAtMs = millis() + presencePollIntervalMs;
      }
      else
      {
        nextPresenceQueryAtMs = millis() + presenceRetryIntervalMs;
      }
    }
  }

  delay(1000);
}

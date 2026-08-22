// For basic ESP32 stuff like wifi, OTA Update and Wifi Manager Server
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include <OneWire.h>
#include <DallasTemperature.h>

// For Display and I2C
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// Data Storage
#include <Preferences.h>

// Date and time functions using a DS1307 RTC connected via I2C and Wire lib
#include "RTClib.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

// RGB LED (2812B)
#include <Adafruit_NeoPixel.h>

#define NUM_RELAYS 4
#define ONE_WIRE_BUS 4
#define TEMPERATURE_HYSTERESIS 0.5f
const uint8_t RELAY_PINS[NUM_RELAYS] = {26, 27, 14, 12};
#define SW_VERSION "v0.3.0-beta"

Preferences preferences;
AsyncWebServer server(80);
RTC_DS3231 rtc;
WiFiUDP ntpUDP;
String ntpPoolServer = "pool.ntp.org";
String customNtpServer;
long timeZoneOffset = 0;
NTPClient timeClient(ntpUDP, ntpPoolServer.c_str(), timeZoneOffset);
bool restartRequested = false;
unsigned long restartAt = 0;
bool wifiSetupMode = false;
bool rtcReady = false;
bool otaActive = false;
unsigned long otaProgressMillis = 0;
String errorBuffer;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress sensorAddresses[8];
uint8_t sensorCount = 0;
float sensorTemperatures[8] = {NAN};
unsigned long lastTemperatureRequest = 0;

bool enableWiFi()
{
  return WiFi.status() == WL_CONNECTED;
}

bool autoTimeUpdate()
{
  Serial.println("[RTC] Time update requested");

  if (!rtcReady || !rtc.begin())
  {
    errorBuffer = "RTC not found. Time update cancelled.";
    Serial.println("[RTC] Update failed: RTC is not available");
    return false;
  }

  if (!enableWiFi())
  {
    errorBuffer = "WiFi is not connected. RTC time update failed.";
    Serial.println("[RTC] Update failed: WiFi is not connected");
    return false;
  }

  String serverName = ntpPoolServer == "custom" ? customNtpServer : ntpPoolServer;
  if (serverName.isEmpty())
    serverName = "pool.ntp.org";

  Serial.printf("[RTC] Updating from NTP server: %s, offset: %ld seconds\n", serverName.c_str(), timeZoneOffset);
  timeClient.setPoolServerName(serverName.c_str());
  timeClient.setTimeOffset(timeZoneOffset);
  timeClient.begin();

  if (!timeClient.update() || !timeClient.isTimeSet())
  {
    errorBuffer = "NTP update failed. RTC time was not changed.";
    Serial.println("[RTC] Update failed: NTP time is not available");
    return false;
  }

  time_t rawTime = timeClient.getEpochTime();
  if (rawTime < 1000000000UL)
  {
    errorBuffer = "NTP returned an invalid time.";
    Serial.println("[RTC] Update failed: invalid NTP epoch");
    return false;
  }

  struct tm timeInfo;
  localtime_r(&rawTime, &timeInfo);
  DateTime updatedTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                       timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  rtc.adjust(updatedTime);

  char message[80];
  snprintf(message, sizeof(message), "RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
           updatedTime.year(), updatedTime.month(), updatedTime.day(),
           updatedTime.hour(), updatedTime.minute(), updatedTime.second());
  errorBuffer = message;
  Serial.printf("[RTC] %s\n", message);
  return true;
}

void onOTAStart()
{
  otaActive = true;
  Serial.println("OTA update started");
}

void onOTAProgress(size_t current, size_t total)
{
  if (millis() - otaProgressMillis < 500)
    return;
  otaProgressMillis = millis();
  Serial.printf("OTA progress: %u/%u bytes\n", current, total);
}

void onOTAEnd(bool success)
{
  otaActive = false;
  Serial.println(success ? "OTA update finished" : "OTA update failed");
}

class Relay
{
private:
  uint8_t pin;
  uint8_t number;
  bool enabled = true;
  bool state = false;
  String name;
  String mode = "manual";
  uint16_t onTime = 0;
  uint16_t offTime = 0;
  bool timerActive = false;
  uint32_t timerStarted = 0;
  uint32_t timerDuration = 0;
  uint16_t toggleOnMinutes = 0;
  uint16_t toggleOffMinutes = 0;
  bool toggleActive = false;
  uint32_t toggleStarted = 0;
  String sensorAddress;
  float targetTemperature = 25.0f;
  float currentTemperature = NAN;
  bool sensorErrorReported = false;

  String path() const { return "/config/relay" + String(number) + ".json"; }

  String addressText(const DeviceAddress address) const
  {
    String value;
    for (uint8_t i = 0; i < 8; i++)
    {
      if (address[i] < 16)
        value += "0";
      value += String(address[i], HEX);
    }
    value.toUpperCase();
    return value;
  }

  void save()
  {
    JsonDocument doc;
    doc["enabled"] = enabled;
    doc["state"] = state;
    doc["name"] = name;
    doc["mode"] = mode;
    doc["onTime"] = onTime;
    doc["offTime"] = offTime;
    doc["toggleOnMinutes"] = toggleOnMinutes;
    doc["toggleOffMinutes"] = toggleOffMinutes;
    doc["toggleActive"] = toggleActive;
    doc["toggleStarted"] = toggleStarted;
    doc["sensorAddress"] = sensorAddress;
    doc["targetTemperature"] = targetTemperature;

    File file = LittleFS.open(path(), "w");
    if (file)
    {
      serializeJson(doc, file);
      file.close();
      Serial.printf("[Relay %u] Configuration saved\n", number);
    }
    else
      Serial.printf("[Relay %u] ERROR: Could not save %s\n", number, path().c_str());
  }

  void applyState(bool newState)
  {
    bool nextState = enabled && newState;
    if (state != nextState)
      Serial.printf("[Relay %u] State: %s -> %s\n", number, state ? "ON" : "OFF", nextState ? "ON" : "OFF");
    state = nextState;
    digitalWrite(pin, state ? HIGH : LOW);
  }

public:
  Relay(uint8_t relayPin, uint8_t relayNumber) : pin(relayPin), number(relayNumber)
  {
    pinMode(pin, OUTPUT);
    name = "Relay " + String(number);
    load();
    applyState(state);
  }

  void load()
  {
    if (!LittleFS.exists(path()))
    {
      Serial.printf("[Relay %u] No saved configuration, using defaults\n", number);
      return;
    }

    File file = LittleFS.open(path(), "r");
    if (!file)
    {
      Serial.printf("[Relay %u] ERROR: Could not open %s\n", number, path().c_str());
      return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error)
    {
      Serial.printf("[Relay %u] ERROR: Invalid configuration JSON: %s\n", number, error.c_str());
      return;
    }

    if (doc["enabled"].is<bool>())
      enabled = doc["enabled"].as<bool>();
    else if (doc["isDisabled"].is<bool>())
      enabled = !doc["isDisabled"].as<bool>();
    if (doc["state"].is<bool>())
      state = doc["state"].as<bool>();
    else
      state = doc["isOn"] | false;
    String defaultName = "Relay " + String(number);
    name = doc["name"] | defaultName;
    mode = doc["mode"] | "manual";
    onTime = doc["onTime"] | 0;
    offTime = doc["offTime"] | 0;
    toggleOnMinutes = doc["toggleOnMinutes"] | 0;
    toggleOffMinutes = doc["toggleOffMinutes"] | 0;
    toggleActive = doc["toggleActive"] | false;
    toggleStarted = doc["toggleStarted"] | 0;
    sensorAddress = doc["sensorAddress"] | "";
    if (doc["targetTemperature"].is<float>())
      targetTemperature = doc["targetTemperature"].as<float>();
    else if (doc["temperatureOn"].is<float>() && doc["temperatureOff"].is<float>())
      targetTemperature = (doc["temperatureOn"].as<float>() + doc["temperatureOff"].as<float>()) / 2.0f;
    timerActive = false;
    Serial.printf("[Relay %u] Loaded: name=%s, mode=%s, enabled=%s, state=%s\n",
                  number, name.c_str(), mode.c_str(), enabled ? "yes" : "no", state ? "ON" : "OFF");
  }

  bool isEnabled() const { return enabled; }
  bool getState() const { return state; }
  String getName() const { return name; }
  String getMode() const { return mode; }
  uint16_t getOnTime() const { return onTime; }
  uint16_t getOffTime() const { return offTime; }
  bool isTimerActive() const { return timerActive; }
  uint32_t getTimerDuration() const { return timerDuration; }
  bool isToggleActive() const { return toggleActive; }
  uint16_t getToggleOnMinutes() const { return toggleOnMinutes; }
  uint16_t getToggleOffMinutes() const { return toggleOffMinutes; }
  String getSensorAddress() const { return sensorAddress; }
  float getTargetTemperature() const { return targetTemperature; }
  float getCurrentTemperature() const { return currentTemperature; }

  uint32_t remainingTimer() const
  {
    if (!timerActive)
      return 0;
    uint32_t elapsed = (millis() - timerStarted) / 1000;
    return elapsed >= timerDuration ? 0 : timerDuration - elapsed;
  }

  uint32_t remainingToggle() const
  {
    uint32_t cycle = (toggleOnMinutes + toggleOffMinutes) * 60UL;
    if (!toggleActive || cycle == 0)
      return 0;
    uint32_t position = ((millis() / 1000) - toggleStarted) % cycle;
    uint32_t boundary = state ? toggleOnMinutes * 60UL : cycle;
    return state ? boundary - position : cycle - position;
  }

  void setName(const String &value)
  {
    Serial.printf("[Relay %u] Name: %s -> %s\n", number, name.c_str(), value.c_str());
    name = value;
    save();
  }

  void setEnabled(bool value)
  {
    Serial.printf("[Relay %u] %s\n", number, value ? "Enabled" : "Disabled");
    enabled = value;
    if (!enabled)
      stopTimer(false);
    applyState(enabled && state);
    save();
  }

  void setMode(const String &value)
  {
    if (value != "manual" && value != "auto" && value != "timer" && value != "toggle" && value != "temperature")
    {
      Serial.printf("[Relay %u] ERROR: Invalid mode '%s'\n", number, value.c_str());
      return;
    }
    Serial.printf("[Relay %u] Mode: %s -> %s\n", number, mode.c_str(), value.c_str());
    if (value != "timer")
      stopTimer(true);
    if (value != "toggle")
      toggleActive = false;
    mode = value;
    save();
  }

  void toggle()
  {
    if (!enabled)
    {
      Serial.printf("[Relay %u] Toggle ignored: relay disabled\n", number);
      return;
    }
    Serial.printf("[Relay %u] Manual toggle requested\n", number);
    applyState(!state);
    save();
  }

  void setScheduledState(bool newState)
  {
    if (enabled)
    {
      Serial.printf("[Relay %u] Auto schedule changed output to %s\n", number, newState ? "ON" : "OFF");
      applyState(newState);
    }
  }

  void setSchedule(uint16_t on, uint16_t off)
  {
    Serial.printf("[Relay %u] Schedule: %04u -> %04u, %04u -> %04u\n", number, onTime, on, offTime, off);
    onTime = on;
    offTime = off;
    save();
  }

  void setTimer(uint32_t duration, bool start)
  {
    Serial.printf("[Relay %u] Timer request: %s, duration=%lu seconds\n", number, start ? "START" : "STOP", duration);
    if (!start || duration == 0)
    {
      stopTimer(false);
      return;
    }
    timerDuration = duration;
    timerStarted = millis();
    timerActive = true;
    save();
  }

  void stopTimer(bool keepState)
  {
    if (timerActive)
      Serial.printf("[Relay %u] Timer stopped (keep state=%s)\n", number, keepState ? "yes" : "no");
    timerActive = false;
    timerDuration = 0;
    if (!keepState)
      applyState(false);
  }

  void setToggle(uint16_t onMinutes, uint16_t offMinutes, bool start)
  {
    Serial.printf("[Relay %u] Toggle mode: %s, ON=%u min, OFF=%u min\n",
                  number, start ? "START" : "STOP", onMinutes, offMinutes);
    toggleOnMinutes = onMinutes;
    toggleOffMinutes = offMinutes;
    toggleActive = start && (onMinutes + offMinutes > 0);
    toggleStarted = millis() / 1000;
    if (toggleActive)
      applyState(true);
    save();
  }

  void setTemperatureConfig(const String &address, float target)
  {
    sensorAddress = address;
    sensorAddress.toUpperCase();
    targetTemperature = target;
    sensorErrorReported = false;
    Serial.printf("[Relay %u] Temperature control: sensor=%s, target=%.2f C, hysteresis=+/- %.2f C\n",
                  number, sensorAddress.c_str(), targetTemperature, TEMPERATURE_HYSTERESIS);
    save();
  }

  void updateTemperature()
  {
    if (mode != "temperature" || !enabled || sensorAddress.isEmpty())
      return;

    int sensorIndex = -1;
    for (uint8_t i = 0; i < sensorCount; i++)
    {
      if (addressText(sensorAddresses[i]) == sensorAddress)
      {
        sensorIndex = i;
        break;
      }
    }
    if (sensorIndex < 0)
    {
      currentTemperature = NAN;
      if (!sensorErrorReported)
      {
        errorBuffer = "Relay " + String(number) + " sensor not found.";
        Serial.printf("[Relay %u] ERROR: Assigned DS18B20 sensor not found\n", number);
        sensorErrorReported = true;
      }
      return;
    }

    currentTemperature = sensorTemperatures[sensorIndex];
    if (currentTemperature == DEVICE_DISCONNECTED_C || isnan(currentTemperature))
    {
      if (!sensorErrorReported)
      {
        errorBuffer = "Relay " + String(number) + " sensor read failed.";
        Serial.printf("[Relay %u] ERROR: DS18B20 temperature read failed\n", number);
        sensorErrorReported = true;
      }
      return;
    }
    sensorErrorReported = false;

    float turnOnAt = targetTemperature - TEMPERATURE_HYSTERESIS;
    float turnOffAt = targetTemperature + TEMPERATURE_HYSTERESIS;
    bool shouldBeOn = state ? currentTemperature < turnOffAt : currentTemperature <= turnOnAt;
    if (shouldBeOn != state)
    {
      Serial.printf("[Relay %u] Temperature %.2f C changed output to %s (ON <= %.2f, OFF >= %.2f)\n",
                    number, currentTemperature, shouldBeOn ? "ON" : "OFF", turnOnAt, turnOffAt);
      applyState(shouldBeOn);
      save();
    }
  }

  bool shouldBeOnNow()
  {
    if (!rtcReady)
      return state;
    DateTime now = rtc.now();
    uint16_t current = now.hour() * 100 + now.minute();
    return offTime > onTime ? current >= onTime && current < offTime
                            : current >= onTime || current < offTime;
  }

  void update()
  {
    if (timerActive && remainingTimer() == 0)
    {
      Serial.printf("[Relay %u] Timer expired, toggling output\n", number);
      timerActive = false;
      toggle();
      timerDuration = 0;
      save();
    }

    if (toggleActive)
    {
      uint32_t cycle = (toggleOnMinutes + toggleOffMinutes) * 60UL;
      if (cycle > 0)
      {
        uint32_t position = ((millis() / 1000) - toggleStarted) % cycle;
        bool shouldBeOn = position < toggleOnMinutes * 60UL;
        if (shouldBeOn != state)
        {
          Serial.printf("[Relay %u] Toggle cycle changed output to %s\n", number, shouldBeOn ? "ON" : "OFF");
          applyState(shouldBeOn);
        }
      }
    }
  }
};

Relay *relays[NUM_RELAYS];

String jsonResponse(JsonDocument &doc)
{
  String response;
  serializeJson(doc, response);
  return response;
}

void setupWifi()
{
  Serial.println("Setting up Wifi");
  preferences.begin("wifi", false);
  String savedSsid = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("password", "");

  if (savedSsid.isEmpty() || savedPassword.isEmpty())
  {
    Serial.println("No saved wifi credentials, starting access point");
    static AsyncWebServer server(80);

    LittleFS.begin(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Smart-Aquarium");
    Serial.print("Wifi setup AP IP address: ");
    Serial.println(WiFi.softAPIP());
    server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request)
              {
      if (!request->hasParam("ssid", true) || !request->hasParam("pass", true))
      {
        request->send(400, "text/plain", "missing wifi details");
        return;
      }

      String ssid = request->getParam("ssid", true)->value();
      String password = request->getParam("pass", true)->value();
      preferences.begin("wifi", false);
      bool saved = preferences.putString("ssid", ssid) > 0;
      saved = preferences.putString("password", password) > 0 && saved;
      preferences.end();

      if (saved)
      {
        Serial.println("Wifi credentials saved, restarting");
        request->send(200, "text/plain", "success");
        restartRequested = true;
        restartAt = millis() + 3000;
      }
      else
      {
        Serial.println("Failed to save wifi credentials");
        request->send(500, "text/plain", "failed to save wifi details");
      } });
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(LittleFS, "/wifimanager.html", "text/html"); });
    server.begin();
  }
  else
  {
    Serial.println("Connecting to saved wifi credentials");
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    unsigned long connectionStartedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectionStartedAt < 15000)
    {
      delay(100);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("Wifi connected IP address: ");
      Serial.println(WiFi.localIP());
    }
    else
    {
      Serial.println("Wifi connection failed");
    }
  }
  preferences.end();
}

void setupRelayApi(uint8_t relayNumber)
{
  uint8_t index = relayNumber - 1;
  String base = "/api/led" + String(relayNumber);

  server.on((base + "/name").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", relays[index]->getName()); });
  server.on((base + "/name").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["name"].is<const char *>())
              {
                request->send(400, "text/plain", "Invalid request");
                return;
              }
              relays[index]->setName(doc["name"].as<String>());
              request->send(200, "text/plain", "Name updated");
            });

  server.on((base + "/status").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", relays[index]->getState() ? "ON" : "OFF"); });
  server.on((base + "/system/state").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["success"] = true;
              doc["enabled"] = relays[index]->isEnabled();
              doc["disabled"] = !relays[index]->isEnabled();
              doc["state"] = relays[index]->getState() ? "ON" : "OFF";
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on((base + "/system/state").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || (!doc["enabled"].is<bool>() && !doc["disabled"].is<bool>()))
              {
                request->send(400, "application/json", "{\"success\":false}");
                return;
              }
              bool enabled = doc["enabled"].is<bool>() ? doc["enabled"].as<bool>() : !doc["disabled"].as<bool>();
              relays[index]->setEnabled(enabled);
              request->send(200, "application/json", "{\"success\":true}");
            });

  server.on((base + "/mode").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", relays[index]->getMode()); });
  server.on((base + "/mode").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["mode"].is<const char *>())
              {
                request->send(400, "text/plain", "Invalid mode");
                return;
              }
              relays[index]->setMode(doc["mode"].as<String>());
              request->send(200, "text/plain", "Mode updated");
            });
  server.on((base + "/toggle").c_str(), HTTP_POST, [index](AsyncWebServerRequest *request)
            {
              relays[index]->toggle();
              request->send(200, "application/json", relays[index]->getState() ? "{\"state\":\"ON\"}" : "{\"state\":\"OFF\"}"); });

  server.on((base + "/schedule").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["onTime"] = relays[index]->getOnTime();
              doc["offTime"] = relays[index]->getOffTime();
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on((base + "/schedule").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["onTime"].is<const char *>() || !doc["offTime"].is<const char *>())
              {
                request->send(400, "application/json", "{\"success\":false}");
                return;
              }
              String on = doc["onTime"].as<String>();
              String off = doc["offTime"].as<String>();
              if (on.length() != 5 || off.length() != 5 || on[2] != ':' || off[2] != ':')
              {
                request->send(400, "application/json", "{\"success\":false}");
                return;
              }
              relays[index]->setMode("auto");
              relays[index]->setSchedule(on.substring(0, 2).toInt() * 100 + on.substring(3).toInt(), off.substring(0, 2).toInt() * 100 + off.substring(3).toInt());
              request->send(200, "application/json", "{\"success\":true}");
            });

  server.on((base + "/timer").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["duration"].is<int>())
              {
                request->send(400, "text/plain", "Invalid timer parameters");
                return;
              }
              bool start = doc["start"].is<bool>() ? doc["start"].as<bool>() : doc["state"] | false;
              relays[index]->setTimer(doc["duration"].as<uint32_t>(), start);
              request->send(200, "text/plain", start ? "Timer started" : "Timer stopped");
            });
  server.on((base + "/timer/state").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["success"] = true;
              doc["active"] = relays[index]->isTimerActive();
              doc["duration"] = relays[index]->getTimerDuration();
              doc["remaining"] = relays[index]->remainingTimer();
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });

  server.on((base + "/temperature").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["sensor"] = relays[index]->getSensorAddress();
              doc["targetTemperature"] = relays[index]->getTargetTemperature();
              doc["temperature"] = relays[index]->getCurrentTemperature();
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on((base + "/temperature").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["sensor"].is<const char *>() ||
                  !doc["targetTemperature"].is<float>())
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid temperature settings\"}");
                return;
              }
              float targetTemperature = doc["targetTemperature"].as<float>();
              if (targetTemperature < -55.0f || targetTemperature > 125.0f)
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Target temperature is outside DS18B20 range\"}");
                return;
              }
              String sensorAddress = doc["sensor"].as<String>();
              sensorAddress.toUpperCase();
              bool sensorFound = false;
              for (uint8_t i = 0; i < sensorCount; i++)
              {
                String address;
                for (uint8_t byteIndex = 0; byteIndex < 8; byteIndex++)
                {
                  if (sensorAddresses[i][byteIndex] < 16)
                    address += "0";
                  address += String(sensorAddresses[i][byteIndex], HEX);
                }
                address.toUpperCase();
                if (address == sensorAddress)
                  sensorFound = true;
              }
              if (!sensorFound)
              {
                errorBuffer = "Selected DS18B20 sensor was not found.";
                Serial.println("[DS18B20] ERROR: Relay assignment rejected for unknown sensor");
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Sensor not found\"}");
                return;
              }
              relays[index]->setMode("temperature");
              relays[index]->setTemperatureConfig(sensorAddress, targetTemperature);
              request->send(200, "application/json", "{\"success\":true}");
            });

  server.on((base + "/toggle-mode").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument doc;
              if (deserializeJson(doc, data, length) || !doc["onMinutes"].is<int>() || !doc["offMinutes"].is<int>() || !doc["start"].is<bool>())
              {
                request->send(400, "application/json", "{\"success\":false}");
                return;
              }
              relays[index]->setMode("toggle");
              relays[index]->setToggle(doc["onMinutes"], doc["offMinutes"], doc["start"]);
              request->send(200, "application/json", "{\"success\":true}");
            });
  server.on((base + "/toggle-mode/state").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["success"] = true;
              doc["active"] = relays[index]->isToggleActive();
              doc["onMinutes"] = relays[index]->getToggleOnMinutes();
              doc["offMinutes"] = relays[index]->getToggleOffMinutes();
              doc["remaining"] = relays[index]->remainingToggle();
              doc["state"] = relays[index]->getState() ? "ON" : "OFF";
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
}

void setupServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(LittleFS, wifiSetupMode ? "/wifimanager.html" : "/index.html", "text/html"); });
  server.serveStatic("/", LittleFS, "/");
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "true"); });
  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", SW_VERSION); });
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              JsonArray list = doc["sensors"].to<JsonArray>();
              for (uint8_t i = 0; i < sensorCount; i++)
              {
                String address;
                for (uint8_t byteIndex = 0; byteIndex < 8; byteIndex++)
                {
                  if (sensorAddresses[i][byteIndex] < 16)
                    address += "0";
                  address += String(sensorAddresses[i][byteIndex], HEX);
                }
                address.toUpperCase();
                JsonObject sensor = list.add<JsonObject>();
                sensor["address"] = address;
                sensor["temperature"] = sensorTemperatures[i];
              }
              doc["count"] = sensorCount;
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on("/api/rtctime", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              if (!rtcReady || !rtc.begin())
              {
                request->send(503, "text/plain", "RTC Error");
                return;
              }
              DateTime now = rtc.now();
              char value[40];
              snprintf(value, sizeof(value), "%02d:%02d %02d/%02d/%04d %.2f",
                       now.hour(), now.minute(), now.day(), now.month(), now.year(), rtc.getTemperature());
              request->send(200, "text/plain", value); });
  server.on("/api/time/update", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("[API] /api/time/update requested");
              bool updated = autoTimeUpdate();
              request->send(updated ? 200 : 500, "text/plain", updated ? "Time updated" : "Time update failed"); });
  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("[API] /api/reset requested");
              for (uint8_t i = 1; i <= NUM_RELAYS; i++)
              {
                String relayFile = "/config/relay" + String(i) + ".json";
                if (LittleFS.exists(relayFile))
                  LittleFS.remove(relayFile);
              }
              preferences.begin("wifi", false);
              preferences.clear();
              preferences.end();
              restartRequested = true;
              restartAt = millis() + 5000;
              request->send(200, "application/json", "{\"success\":true,\"info\":\"Reset complete. Device will reboot in 5 seconds.\"}"); });
  server.on("/api/error", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["error"] = errorBuffer;
              String response = jsonResponse(doc);
              request->send(200, "application/json", response);
              errorBuffer = ""; });
  for (uint8_t i = 1; i <= NUM_RELAYS; i++)
    setupRelayApi(i);
  server.begin();
}

TaskHandle_t loop2Code;
void setup(void)
{
  Serial.begin(115200);
  Serial.println("Starting Smart Aquarium V4.0");

  Serial.println("Initializing LittleFS");
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed");
    errorBuffer = "LittleFS mount failed. Configuration may not be saved.";
  }
  if (!LittleFS.exists("/config"))
    LittleFS.mkdir("/config");

  Serial.println("Initializing RTC");
  rtcReady = rtc.begin();
  if (!rtcReady)
  {
    Serial.println("RTC not found");
    errorBuffer = "RTC not found. Time functions will be unavailable.";
  }

  Serial.printf("[DS18B20] Searching on GPIO %u\n", ONE_WIRE_BUS);
  sensors.begin();
  sensorCount = min(static_cast<uint8_t>(sensors.getDeviceCount()), static_cast<uint8_t>(8));
  for (uint8_t i = 0; i < sensorCount; i++)
  {
    if (sensors.getAddress(sensorAddresses[i], i))
    {
      Serial.printf("[DS18B20] Sensor %u found: ", i + 1);
      for (uint8_t byteIndex = 0; byteIndex < 8; byteIndex++)
      {
        if (sensorAddresses[i][byteIndex] < 16)
          Serial.print("0");
        Serial.print(sensorAddresses[i][byteIndex], HEX);
      }
      Serial.println();
    }
  }
  if (sensorCount == 0)
  {
    errorBuffer = "No DS18B20 sensors found on GPIO " + String(ONE_WIRE_BUS) + ".";
    Serial.println("[DS18B20] ERROR: No sensors found");
  }
  else
  {
    Serial.printf("[DS18B20] %u sensor(s) ready\n", sensorCount);
  }

  Serial.println("Initializing relays");
  for (uint8_t i = 0; i < NUM_RELAYS; i++)
    relays[i] = new Relay(RELAY_PINS[i], i + 1);

  setupWifi();

  if (!wifiSetupMode)
  {
    Serial.println("Setting up server");
    ElegantOTA.begin(&server);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);
    setupServer();
    Serial.println("Server setup complete");
  }

  // task executed in the loop2() function, with priority 1 and executed on core 0
  Serial.println("Creating loop2 task");
  xTaskCreatePinnedToCore(
      loop2,       // Task function.
      "loop2Code", // name of task.
      10000,       // Stack size of task
      NULL,        // parameter of the task
      1,           // priority of the task
      &loop2Code,  // Task handle to keep track of created task
      0);          // pin task to core 0
  Serial.println("Setup complete");
}
void loop2(void *pvParameters)
{
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void loop(void)
{
  ElegantOTA.loop();

  // Keep timer and toggle modes responsive, independent of schedule polling.
  for (uint8_t i = 0; i < NUM_RELAYS; i++)
    relays[i]->update();

  // Check auto relay states and the RTC at most once every two seconds.
  static unsigned long lastScheduleCheck = 0;
  unsigned long currentMillis = millis();
  if (currentMillis - lastScheduleCheck >= 2000)
  {
    lastScheduleCheck = currentMillis;
    if (sensorCount > 0)
    {
      sensors.requestTemperatures();
      for (uint8_t i = 0; i < sensorCount; i++)
        sensorTemperatures[i] = sensors.getTempC(sensorAddresses[i]);
    }
    for (uint8_t i = 0; i < NUM_RELAYS; i++)
    {
      if (relays[i]->getMode() == "auto" && relays[i]->isEnabled())
      {
        bool scheduled = relays[i]->shouldBeOnNow();
        if (scheduled != relays[i]->getState())
          relays[i]->setScheduledState(scheduled);
      }
      relays[i]->updateTemperature();
    }
  }

  if (restartRequested && (long)(millis() - restartAt) >= 0)
  {
    Serial.println("[System] Restarting now");
    ESP.restart();
  }
  else if (restartRequested)
  {
    static unsigned long lastRestartMessage = 0;
    if (millis() - lastRestartMessage >= 1000)
    {
      lastRestartMessage = millis();
      unsigned long remaining = (restartAt - millis() + 999) / 1000;
      Serial.printf("[System] Restarting in %lu second%s\n", remaining, remaining == 1 ? "" : "s");
    }
  }
}
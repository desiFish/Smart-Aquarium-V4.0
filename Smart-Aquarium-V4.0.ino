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
#include <nvs_flash.h>

// Date and time functions using a DS1307 RTC connected via I2C and Wire lib
#include "RTClib.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

// RGB LED (2812B)
#include <Adafruit_NeoPixel.h>

/** Number of relay channels managed by the aquarium controller. */
#define NUM_RELAYS 4
/** GPIO pin connected to the DS18B20 sensor bus. */
#define ONE_WIRE_BUS 4
/** Temperature dead-band used for relay control hysteresis. */
#define TEMPERATURE_HYSTERESIS 0.5f
/** GPIO pin that drives the status LED (WS2812B). */
#define LED_PIN 19
/** GPIO pin that drives the buzzer. */
#define BUZZER_PIN 18
/** GPIO pin for the left user button. */
#define BUTTON_LEFT 36
/** GPIO pin for the right user button. */
#define BUTTON_RIGHT 39
/** I2C address of the RTC module. */
#define RTC_I2C_ADDRESS 0x68
/** Relay output polarity setting for the board. */
#define RELAY_ACTIVE_LOW true
/** Physical relay output pins in board order. */
const uint8_t RELAY_PINS[NUM_RELAYS] = {32, 33, 25, 26};
/** Current firmware version string exposed by the web API. */
#define SW_VERSION "v0.4.2-beta"

/** RGB status LED driver instance. */
Adafruit_NeoPixel statusLed(1, LED_PIN, NEO_GRB + NEO_KHZ800);

/** Debounce time in milliseconds for button polling. */
const uint8_t buttonDebounceMs = 30;

/** Persistent configuration store used for settings and calibration values. */
Preferences preferences;
/** Async web server instance used for the control dashboard and API. */
AsyncWebServer server(80);
/** Real-time clock instance used for scheduling and RTC time checks. */
RTC_DS3231 rtc;
/** UDP socket used for NTP time synchronization. */
WiFiUDP ntpUDP;
/** Primary NTP pool hostname configured for time sync. */
String ntpPoolServer = "in.pool.ntp.org";
/** Custom NTP pool value stored when the user selects a custom server. */
String customNtpServer;
/** Timezone offset in seconds for RTC/NTP synchronization. */
long timeZoneOffset = 19800;
/** NTP client configured with the current pool and timezone offset. */
NTPClient timeClient(ntpUDP, ntpPoolServer.c_str(), timeZoneOffset);
/** True when a firmware reboot or restart should be triggered. */
bool restartRequested = false;
/** Target millisecond timestamp when a pending restart should execute. */
unsigned long restartAt = 0;
/** True while the device is running in Wi-Fi access-point configuration mode. */
bool wifiSetupMode = false;
/** True while a factory reset sequence is in progress. */
bool resetAll = false;
/** RTC availability flag updated by the I2C health check. */
bool rtcReady = false;
/** Prevents repeated RTC alarm notifications while the RTC remains unhealthy. */
bool rtcFailureReported = false;
/** Prevents repeated OLED alarm notifications while the OLED remains unhealthy. */
bool oledFailureReported = false;
/** True when the system should make a reboot or OTA action to persist changes. */
bool useTempSensor = true;
/** Global alarm latch used for repeated buzzer warnings while sensor faults remain active. */
volatile bool temperatureReadFailureAlarm = false;
/** Counter tracking how many temperature-sensor failures are currently active. */
uint8_t temperatureReadFailureCount = 0;
/** Human-readable suffix list for currently failing temperature sensors. */
String temperatureReadFailureNames;
/** Last OTA progress timestamp to throttle serial logging. */
unsigned long otaProgressMillis = 0;
/** Buffer of accumulated operational errors exposed via the API. */
String errorBuffer;
/** DS18B20 bus driver used for temperature monitoring. */
OneWire oneWire(ONE_WIRE_BUS);
/** DallasTemperature sensor manager instance for the 1-Wire bus. */
DallasTemperature sensors(&oneWire);
/** Detected DS18B20 addresses stored by sensor index. */
DeviceAddress sensorAddresses[8];
/** Number of temperature sensors found on the bus. */
uint8_t sensorCount = 0;
/** Last temperatures read for each discovered sensor. */
float sensorTemperatures[8] = {NAN};

/** I2C address of the SH1106 OLED display module. */
#define i2c_Address 0x3c
/** OLED display width in pixels. */
#define SCREEN_WIDTH 128
/** OLED display height in pixels. */
#define SCREEN_HEIGHT 64
/** Reset pin for the OLED; -1 means the board uses the shared reset line. */
#define OLED_RESET -1
/** OLED display controller instance connected via I2C. */
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

/** Idle timeout after which the display powers off when no user interaction is detected. */
const uint32_t DISPLAY_INACTIVITY_TIMEOUT = 30000UL;
/** Current power state of the OLED panel. */
bool displayIsOn = true;
/** True when the OLED responds successfully to I2C traffic. */
bool displayReady = false;
/** Last millisecond timestamp when any button input was detected. */
unsigned long lastButtonPressTime = 0;

/** Maximum queued display messages kept for the UI message queue. */
#define DISPLAY_MESSAGE_QUEUE_SIZE 6
/** Ring buffer holding queued display messages. */
String displayMessageQueue[DISPLAY_MESSAGE_QUEUE_SIZE];
/** Number of messages currently stored in the queue. */
uint8_t displayMessageQueueCount = 0;
/** Current message being shown in the UI. */
String activeDisplayMessage = "";
/** True when a display message is currently active. */
bool displayMessageActive = false;

/**
 * Turns the OLED panel on or off by writing the SH1106 power command over I2C.
 *
 * @param state True to power the display on, false to shut it down.
 */
void displayPower(bool state)
{
  static bool currentState = true;
  if (currentState == state)
    return;

  Wire.beginTransmission(0x3C);
  Wire.write(0x00);
  Wire.write(state ? 0xAF : 0xAE);
  Wire.endTransmission();
  currentState = state;
}

/**
 * Marks the display as active so it wakes up if it was previously turned off.
 */
void markDisplayActivity()
{
  if (!displayIsOn)
  {
    displayIsOn = true;
    displayPower(true);
  }
}

/**
 * Draws the Wi-Fi signal indicator using up to five bars in the top-right corner.
 *
 * @param signalBars Number of bars to render, from 0 to 5.
 */
void drawWiFiSignal(uint8_t signalBars)
{
  const int xPositions[5] = {96, 102, 108, 114, 120};
  const int y = 2;
  const int radius = 2;

  for (uint8_t i = 0; i < 5; i++)
  {
    if (i < signalBars)
      display.fillCircle(xPositions[i], y, radius, SH110X_WHITE);
    else
      display.drawCircle(xPositions[i], y, radius, SH110X_WHITE);
  }
}

/**
 * Adds a message to the display queue when the UI is in a transient message state.
 *
 * @param message Message text to queue for display.
 */
void queueDisplayMessage(const String &message)
{
  if (message.isEmpty())
    return;

  if (activeDisplayMessage == message)
    return;

  for (uint8_t i = 0; i < displayMessageQueueCount; i++)
  {
    if (displayMessageQueue[i] == message)
      return;
  }

  if (displayMessageQueueCount >= DISPLAY_MESSAGE_QUEUE_SIZE)
  {
    for (uint8_t i = 0; i < DISPLAY_MESSAGE_QUEUE_SIZE - 1; i++)
      displayMessageQueue[i] = displayMessageQueue[i + 1];
    displayMessageQueueCount = DISPLAY_MESSAGE_QUEUE_SIZE - 1;
  }

  displayMessageQueue[displayMessageQueueCount++] = message;

  if (!displayMessageActive)
  {
    activeDisplayMessage = displayMessageQueue[0];
    for (uint8_t i = 0; i < displayMessageQueueCount - 1; i++)
      displayMessageQueue[i] = displayMessageQueue[i + 1];
    displayMessageQueueCount--;
    displayMessageActive = true;
  }
}

/**
 * Clears the current display message and advances the queued message, if any.
 */
void dismissDisplayMessage()
{
  if (!displayMessageActive)
    return;

  if (displayMessageQueueCount > 0)
  {
    activeDisplayMessage = displayMessageQueue[0];
    for (uint8_t i = 0; i < displayMessageQueueCount - 1; i++)
      displayMessageQueue[i] = displayMessageQueue[i + 1];
    displayMessageQueueCount--;
    return;
  }

  activeDisplayMessage = "";
  displayMessageActive = false;
}

/**
 * Renders a multi-line message with word wrapping inside the OLED drawing area.
 *
 * @param message Text to render.
 * @param x Left coordinate in pixels.
 * @param y Top coordinate in pixels.
 */
void drawWrappedDisplayMessage(const String &message, int16_t x, int16_t y)
{
  const uint8_t maxLines = 4;
  const uint8_t maxCharacters = (SCREEN_WIDTH - x - 4) / 6;
  String line;
  uint8_t lineCount = 0;

  for (uint16_t i = 0; i <= message.length() && lineCount < maxLines; i++)
  {
    char character = i < message.length() ? message[i] : '\n';
    bool lineBreak = character == '\n';
    if (!lineBreak)
    {
      line += character;
      if (line.length() < maxCharacters)
        continue;
    }

    if (line.length() > maxCharacters)
    {
      int16_t splitAt = line.lastIndexOf(' ', maxCharacters - 1);
      if (splitAt <= 0)
        splitAt = maxCharacters;
      String remainder = line.substring(splitAt);
      line = line.substring(0, splitAt);
      remainder.trim();
      line = line.substring(0, maxCharacters);
      display.setCursor(x, y + lineCount * 8);
      display.println(line);
      lineCount++;
      line = remainder;
      continue;
    }

    line.trim();
    display.setCursor(x, y + lineCount * 8);
    display.println(line);
    lineCount++;
    line = "";
  }
}

/**
 * Draws the main status screen, including Wi-Fi signal strength and active messages.
 */
void drawStatusScreen()
{
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setFont(NULL);

  if (displayMessageActive && !activeDisplayMessage.isEmpty())
  {
    display.setCursor(4, 4);
    display.println("System");
    display.drawFastHLine(0, 14, SCREEN_WIDTH, SH110X_WHITE);

    display.setCursor(4, 20);
    drawWrappedDisplayMessage(activeDisplayMessage, 4, 20);

    display.setCursor(4, 52);
    display.println("Press any button");
    display.display();
    return;
  }

  if (resetAll)
  {
    display.setCursor(8, 4);
    display.println("Resetting All");
    display.setCursor(8, 18);
    display.println("Please wait");
    display.display();
    return;
  }

  if (WiFi.getMode() == WIFI_AP)
  {
    display.setCursor(8, 4);
    display.println("Connect to:");
    display.setCursor(8, 18);
    display.println("Smart-Aquarium");
    display.setCursor(8, 36);
    display.println("AP IP:");
    display.setCursor(8, 48);
    display.println(WiFi.softAPIP().toString());
    display.display();
    return;
  }

  uint8_t signalBars = 0;
  if (WiFi.status() == WL_CONNECTED)
  {
    int32_t rssi = WiFi.RSSI();
    if (rssi >= -50)
      signalBars = 5;
    else if (rssi >= -60)
      signalBars = 4;
    else if (rssi >= -67)
      signalBars = 3;
    else if (rssi >= -75)
      signalBars = 2;
    else if (rssi >= -85)
      signalBars = 1;
  }
  drawWiFiSignal(signalBars);

  display.setCursor(10, (SCREEN_HEIGHT / 2) - 4);
  display.print("IP: ");
  if (WiFi.status() == WL_CONNECTED)
    display.print(WiFi.localIP().toString());
  else
    display.print("Not connected");

  display.display();
}

/**
 * Emits a buzzer pattern for a short alarm or confirmation tone.
 *
 * @param times Number of buzzer cycles.
 * @param delayMs Time in milliseconds between each HIGH/LOW pulse.
 */
void beep(uint8_t times = 1, uint16_t delayMs = 200)
{
  for (uint8_t i = 0; i < times; i++)
  {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(delayMs);
    digitalWrite(BUZZER_PIN, LOW);
    delay(delayMs);
  }
}

/**
 * Flashes the status LED in a given color for a number of cycles.
 *
 * @param color One of RED, GREEN, BLUE, YELLOW, or OFF.
 * @param times Number of flashes to emit.
 * @param delayMs Delay in milliseconds between toggles.
 */
void ledBlink(const String &color, uint8_t times = 1, uint16_t delayMs = 500)
{
  uint32_t rgb = 0;
  if (color == "RED")
    rgb = statusLed.Color(255, 0, 0);
  else if (color == "GREEN")
    rgb = statusLed.Color(0, 255, 0);
  else if (color == "BLUE")
    rgb = statusLed.Color(0, 0, 255);
  else if (color == "YELLOW")
    rgb = statusLed.Color(255, 255, 0);
  else if (color == "OFF")
    rgb = 0;

  for (uint8_t i = 0; i < times; i++)
  {
    statusLed.setPixelColor(0, rgb);
    statusLed.show();
    delay(delayMs);
    statusLed.clear();
    statusLed.show();
    delay(delayMs);
  }
}

/**
 * Appends a formatted error line to the API-visible error buffer.
 *
 * @param message Error text to store.
 */
void addBufferError(const String &message)
{
  if (message.isEmpty())
    return;

  if (!errorBuffer.isEmpty())
    errorBuffer += "\n";
  errorBuffer += message;
}

/**
 * Polls the RTC and OLED over I2C and marks each device as healthy or unhealthy.
 * When a device recovers, the alarm state is cleared and the device is reinitialized when needed.
 */
void checkI2CHealth()
{
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  uint8_t error = Wire.endTransmission();

  if (error != 0)
  {
    rtcReady = false;
    if (!rtcFailureReported)
    {
      rtcFailureReported = true;
      addBufferError("RTC: runtime communication failed.");
      queueDisplayMessage("RTC Failure");
      Serial.printf("[RTC] ERROR: Runtime I2C check failed, error=%u\n", error);
    }
  }
  else if (rtcFailureReported)
  {
    rtcFailureReported = false;
    rtcReady = true;
    addBufferError("RTC communication restored.");
    queueDisplayMessage("RTC Restored");
    Serial.println("[RTC] Runtime I2C communication restored");
  }
  else
  {
    rtcReady = true;
  }

  Wire.beginTransmission(0x3C);
  uint8_t oledError = Wire.endTransmission();

  if (oledError != 0)
  {
    displayReady = false;
    displayIsOn = false;
    if (!oledFailureReported)
    {
      oledFailureReported = true;
      addBufferError("OLED: runtime communication failed.");
      beep(3, 200);
      Serial.printf("[OLED] ERROR: Runtime I2C check failed, error=%u\n", oledError);
    }
  }
  else if (oledFailureReported)
  {
    oledFailureReported = false;
    displayReady = display.begin(i2c_Address, true);
    markDisplayActivity();
    addBufferError("OLED communication restored.");
    beep(1, 150);
    Serial.println("[OLED] Runtime I2C communication restored");
  }
  else
  {
    displayReady = true;
  }
}

/**
 * Loads the stored NTP and timezone configuration from Preferences.
 */
void loadTimeSettings()
{
  preferences.begin("time", false);
  ntpPoolServer = preferences.getString("ntpServer", "in.pool.ntp.org");
  customNtpServer = preferences.getString("customServer", "");
  long savedOffset = preferences.getLong("offset", 19800);
  preferences.end();
  timeZoneOffset = savedOffset;
  Serial.printf("[Time] Loaded offset=%ld, NTP=%s%s%s%s\n",
                timeZoneOffset, ntpPoolServer.c_str(),
                ntpPoolServer == "custom" ? " (" : "",
                ntpPoolServer == "custom" ? customNtpServer.c_str() : "",
                ntpPoolServer == "custom" ? ")" : "");
}

/**
 * Loads the system-wide configuration values from Preferences.
 */
void loadSystemConfig()
{
  Serial.println("[System] Loading system config...");
  useTempSensor = true;
  preferences.begin("system", false);
  if (preferences.isKey("useTempSensor"))
  {
    useTempSensor = preferences.getBool("useTempSensor", true);
    preferences.end();
    Serial.printf("[System] Loaded useTempSensor=%s from Preferences\n", useTempSensor ? "true" : "false");
    return;
  }
  preferences.end();
}

/**
 * Saves the system-wide configuration values to Preferences when changed.
 */
void saveSystemConfig()
{
  Serial.println("[System] Saving system config...");
  Serial.printf("[System] useTempSensor=%s\n", useTempSensor ? "true" : "false");

  preferences.begin("system", false);
  if (preferences.isKey("useTempSensor") && preferences.getBool("useTempSensor", true) == useTempSensor)
  {
    preferences.end();
    Serial.println("[System] Config unchanged; skipping save");
    return;
  }
  bool saved = preferences.putBool("useTempSensor", useTempSensor);
  preferences.end();
  if (saved)
    Serial.println("[System] Successfully saved system config to Preferences");
  else
    Serial.println("[System] ERROR: Could not save system config to Preferences");
}

/**
 * Saves the current time configuration to Preferences unless it has not changed.
 */
void saveTimeSettings()
{
  preferences.begin("time", false);
  bool unchanged = preferences.getString("ntpServer", "pool.ntp.org") == ntpPoolServer &&
                   preferences.getString("customServer", "") == customNtpServer &&
                   preferences.getLong("offset", 19800) == timeZoneOffset;
  if (unchanged)
  {
    preferences.end();
    Serial.println("[Time] Settings unchanged; skipping save");
    return;
  }
  preferences.putString("ntpServer", ntpPoolServer);
  preferences.putString("customServer", customNtpServer);
  preferences.putLong("offset", timeZoneOffset);
  preferences.end();
  Serial.printf("[Time] Saved offset=%ld, NTP=%s%s%s%s\n",
                timeZoneOffset, ntpPoolServer.c_str(),
                ntpPoolServer == "custom" ? " (" : "",
                ntpPoolServer == "custom" ? customNtpServer.c_str() : "",
                ntpPoolServer == "custom" ? ")" : "");
}

/**
 * Returns true when the ESP32 is connected to a Wi-Fi network.
 *
 * @return True if Wi-Fi is connected, false otherwise.
 */
bool enableWiFi()
{
  return WiFi.status() == WL_CONNECTED;
}

/**
 * Synchronizes the RTC from the configured NTP server and updates the stored time metadata.
 *
 * @return True when the RTC was updated successfully, false otherwise.
 */
bool autoTimeUpdate()
{
  Serial.println("[RTC] Time update requested");

  if (!rtcReady)
  {
    addBufferError("RTC not found. Time update cancelled.");
    Serial.println("[RTC] Update failed: RTC is not available");
    return false;
  }

  if (!enableWiFi())
  {
    addBufferError("WiFi is not connected. RTC time update failed.");
    Serial.println("[RTC] Update failed: WiFi is not connected");
    Serial.printf("[RTC] WiFi status=%d, SSID=%s, IP=%s\n", WiFi.status(), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
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
    addBufferError("NTP update failed. RTC time was not changed.");
    Serial.println("[RTC] Update failed: NTP time is not available");
    return false;
  }

  time_t rawTime = timeClient.getEpochTime();
  if (rawTime < 1000000000UL)
  {
    addBufferError("NTP returned an invalid time.");
    Serial.println("[RTC] Update failed: invalid NTP epoch");
    return false;
  }

  struct tm timeInfo;
  localtime_r(&rawTime, &timeInfo);
  DateTime updatedTime(timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
                       timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  rtc.adjust(updatedTime);

  DateTime now = rtc.now();
  preferences.begin("time", false);
  if (preferences.getUChar("lastUpdateDay", 0) != now.day())
    preferences.putUChar("lastUpdateDay", now.day());
  preferences.end();

  char message[80];
  snprintf(message, sizeof(message), "RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
           updatedTime.year(), updatedTime.month(), updatedTime.day(),
           updatedTime.hour(), updatedTime.minute(), updatedTime.second());
  addBufferError(message);
  Serial.printf("[RTC] %s\n", message);
  return true;
}

/**
 * Called when an OTA update begins. Sets the OTA activity flag for external monitoring.
 */
void onOTAStart()
{
  Serial.println("OTA update started");
}

/**
 * Logs OTA upload progress at a throttled interval to avoid flooding the serial console.
 *
 * @param current Number of bytes uploaded so far.
 * @param total Total size of the firmware image.
 */
void onOTAProgress(size_t current, size_t total)
{
  if (millis() - otaProgressMillis < 500)
    return;
  otaProgressMillis = millis();
  Serial.printf("OTA progress: %u/%u bytes\n", current, total);
}

/**
 * Called when OTA finishes and clears the OTA activity state.
 *
 * @param success True when the OTA completed successfully.
 */
void onOTAEnd(bool success)
{
  Serial.println(success ? "OTA update finished" : "OTA update failed");
}

/**
 * Relay class manages a single GPIO-controlled relay with support for four control modes:
 *   - Manual: State is controlled directly via toggle() or setEnabled()
 *   - Auto: State depends on RTC schedule (on-time and off-time)
 *   - Timer: State toggles after a countdown duration
 *   - Toggle: State cycles between ON/OFF at user-configurable intervals
 *   - Temperature: State is driven by DS18B20 sensor feedback with hysteresis
 *
 * Configuration is persisted to LittleFS as JSON, and the relay applies its state
 * to the GPIO pin whenever any operating parameter changes. Each relay maintains:
 *   - Pin number and relay index
 *   - Enabled/disabled flag
 *   - Current output state (ON/OFF)
 *   - Name (for UI and API)
 *   - Mode, schedule times, timer duration, toggle parameters, and sensor config
 *   - Temperature and error tracking for sensor failures
 *
 * Temperature control mode actively monitors a selected sensor and uses hysteresis
 * to prevent relay chatter. When a sensor read fails, the relay switches to manual
 * mode and fires with the failure alarm state until the sensor recovers.
 */
class Relay
{
private:
  uint8_t pin;
  uint8_t number;
  bool enabled = false;
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
  bool temperatureReadFailed = false;

  String path() const { return "/config/relay" + String(number) + ".json"; }

  /**
   * Converts a 1-Wire device address to an uppercase hexadecimal string (16 characters).
   *
   * @param address The 8-byte DeviceAddress array from Dallas sensor library.
   * @return Uppercase hex string like "28AA123456789ABC".
   */
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

  /**
   * Persists the relay's current configuration to a JSON file in LittleFS.
   * File path is /config/relay[N].json where N is the relay number (1-4).
   * Saved fields include enabled, state, name, mode, schedule times, timer and toggle state,
   * temperature sensor address and target temperature.
   */
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

  /**
   * Applies a GPIO output state to the relay pin, respecting the enabled flag.
   * Relays are set OFF when disabled, and actual output is XOR'd with RELAY_ACTIVE_LOW
   * to handle active-high or active-low board designs. Logs state transitions to serial.
   *
   * @param newState Desired logical state (true = ON, false = OFF).
   */
  void applyState(bool newState)
  {
    bool nextState = enabled && newState;
    if (state != nextState)
      Serial.printf("[Relay %u] State: %s -> %s\n", number, state ? "ON" : "OFF", nextState ? "ON" : "OFF");
    state = nextState;
    digitalWrite(pin, (state ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
  }

public:
  /**
   * Constructs a relay object, initializes GPIO, and loads persisted configuration from storage.
   *
   * @param relayPin GPIO pin number connected to the relay driver.
   * @param relayNumber Relay index (1-4) used in configuration file paths and logging.
   */
  Relay(uint8_t relayPin, uint8_t relayNumber) : pin(relayPin), number(relayNumber)
  {
    digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(pin, OUTPUT);
    name = "Relay " + String(number);
    load();
    applyState(state);
  }

  /**
   * Loads relay configuration from the JSON file on LittleFS if it exists.
   * Falls back to default values (disabled, manual mode, all timers stopped) if no file is found.
   * Handles legacy configuration formats from earlier firmware versions for backward compatibility.
   * Reports loaded values to serial console for diagnostics.
   */
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

  /** @return True if the relay is enabled and can activate; false if disabled. */
  bool isEnabled() const { return enabled; }

  /** @return Current logical output state (true = ON, false = OFF). */
  bool getState() const { return state; }

  /** @return User-assigned relay name (e.g., "Heater", "Filter Pump"). */
  String getName() const { return name; }

  /** @return Current control mode: "manual", "auto", "timer", "toggle", or "temperature". */
  String getMode() const { return mode; }

  /** @return Scheduled on-time in HHMM format (e.g., 0900 for 9:00 AM). */
  uint16_t getOnTime() const { return onTime; }

  /** @return Scheduled off-time in HHMM format. */
  uint16_t getOffTime() const { return offTime; }

  /** @return True when a timer countdown is active. */
  bool isTimerActive() const { return timerActive; }

  /** @return Timer duration in seconds when timer mode is active. */
  uint32_t getTimerDuration() const { return timerDuration; }

  /** @return True when toggle cycle is running. */
  bool isToggleActive() const { return toggleActive; }

  /** @return Duration of ON phase in toggle mode in minutes. */
  uint16_t getToggleOnMinutes() const { return toggleOnMinutes; }

  /** @return Duration of OFF phase in toggle mode in minutes. */
  uint16_t getToggleOffMinutes() const { return toggleOffMinutes; }

  /** @return Hexadecimal address of the assigned temperature sensor (e.g., "28AA123456789ABC"). */
  String getSensorAddress() const { return sensorAddress; }

  /** @return Target temperature setpoint in Celsius for temperature control mode. */
  float getTargetTemperature() const { return targetTemperature; }

  /** @return Most recently read temperature from the assigned sensor, or NAN if unavailable. */
  float getCurrentTemperature() const { return currentTemperature; }

  /**
   * Calculates remaining time in the active timer countdown.
   *
   * @return Seconds remaining until timer expires; 0 if timer is inactive or expired.
   */
  uint32_t remainingTimer() const
  {
    if (!timerActive)
      return 0;
    uint32_t elapsed = (millis() - timerStarted) / 1000;
    return elapsed >= timerDuration ? 0 : timerDuration - elapsed;
  }

  /**
   * Calculates remaining time in the current toggle cycle phase (ON or OFF).
   *
   * @return Seconds until toggle switches to the opposite state; 0 if toggle is inactive.
   */
  uint32_t remainingToggle() const
  {
    uint32_t cycle = (toggleOnMinutes + toggleOffMinutes) * 60UL;
    if (!toggleActive || cycle == 0)
      return 0;
    uint32_t position = ((millis() / 1000) - toggleStarted) % cycle;
    uint32_t boundary = state ? toggleOnMinutes * 60UL : cycle;
    return state ? boundary - position : cycle - position;
  }

  /**
   * Updates the relay's human-readable name and persists it to storage.
   *
   * @param value New name string (e.g., "Heater", "Air Pump").
   */
  void setName(const String &value)
  {
    if (name == value)
      return;
    Serial.printf("[Relay %u] Name: %s -> %s\n", number, name.c_str(), value.c_str());
    name = value;
    save();
  }

  /**
   * Enables or disables the relay. Disabled relays cannot be turned ON via any mode.
   * When disabled, any active timer is canceled and the output is forced OFF.
   *
   * @param value True to enable, false to disable.
   */
  void setEnabled(bool value)
  {
    if (enabled == value)
      return;
    Serial.printf("[Relay %u] %s\n", number, value ? "Enabled" : "Disabled");
    enabled = value;
    if (!enabled)
      stopTimer(false);
    applyState(enabled && state);
    save();
  }

  /**
   * Changes the relay's control mode and validates the requested mode.
   * Invalid mode names are rejected with serial logging. When switching modes,
   * existing timer and toggle state are cleared as appropriate.
   *
   * @param value One of "manual", "auto", "timer", "toggle", or "temperature".
   *              If temperature sensors are disabled globally, "temperature" mode is ignored.
   */
  void setMode(const String &value)
  {
    if (value != "manual" && value != "auto" && value != "timer" && value != "toggle" && value != "temperature")
    {
      Serial.printf("[Relay %u] ERROR: Invalid mode '%s'\n", number, value.c_str());
      return;
    }
    if (!useTempSensor && value == "temperature")
    {
      Serial.printf("[Relay %u] Temperature mode ignored: sensors are disabled\n", number);
      return;
    }
    if (mode == value)
      return;
    Serial.printf("[Relay %u] Mode: %s -> %s\n", number, mode.c_str(), value.c_str());
    if (value != "timer")
      stopTimer(true);
    if (value != "toggle")
      toggleActive = false;
    mode = value;
    save();
  }

  /**
   * Manually toggles the relay output state (ON to OFF or vice versa).
   * Only succeeds if the relay is enabled. Changes are persisted to storage.
   */
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

  /**
   * Sets the output state based on the auto-schedule (used by loop2 when evaluating shouldBeOnNow).
   * Only applied if the relay is enabled.
   *
   * @param newState Desired state according to the on-time/off-time schedule.
   */
  void setScheduledState(bool newState)
  {
    if (enabled)
    {
      Serial.printf("[Relay %u] Auto schedule changed output to %s\n", number, newState ? "ON" : "OFF");
      applyState(newState);
    }
  }

  /**
   * Sets the on-time and off-time for auto-schedule mode (HHMM format).
   * Switching to auto mode activates scheduled control based on the RTC clock.
   *
   * @param on Start time in HHMM format (e.g., 0900 for 9:00 AM).
   * @param off Stop time in HHMM format.
   */
  void setSchedule(uint16_t on, uint16_t off)
  {
    if (onTime == on && offTime == off)
      return;
    Serial.printf("[Relay %u] Schedule: %04u -> %04u, %04u -> %04u\n", number, onTime, on, offTime, off);
    onTime = on;
    offTime = off;
    save();
  }

  /**
   * Starts or stops a timer-based relay toggle. When started, the relay automatically toggles
   * its output after the specified duration expires. Stopping a timer leaves the relay in its
   * current state unless keepState is false (then it's turned OFF).
   *
   * @param duration Timer duration in seconds.
   * @param start True to start the countdown; false to stop it.
   */
  void setTimer(uint32_t duration, bool start)
  {
    Serial.printf("[Relay %u] Timer request: %s, duration=%lu seconds\n", number, start ? "START" : "STOP", duration);
    if (!start || duration == 0)
    {
      if (!timerActive && timerDuration == 0)
        return;
      stopTimer(false);
      return;
    }
    if (timerActive && timerDuration == duration)
      return;
    timerDuration = duration;
    timerStarted = millis();
    timerActive = true;
    save();
  }

  /**
   * Internal helper to cancel an active timer and optionally restore a previous state.
   * Logs timer cancellation to serial console.
   *
   * @param keepState If true, leaves the relay in its current ON/OFF state.
   *                  If false, forces the relay OFF.
   */
  void stopTimer(bool keepState)
  {
    if (timerActive)
      Serial.printf("[Relay %u] Timer stopped (keep state=%s)\n", number, keepState ? "yes" : "no");
    timerActive = false;
    timerDuration = 0;
    if (!keepState)
      applyState(false);
  }

  /**
   * Starts or stops a repeating toggle cycle where the relay alternates between ON and OFF
   * at user-configured intervals (in minutes). When started, the relay begins in ON state.
   *
   * @param onMinutes Duration of ON phase in minutes.
   * @param offMinutes Duration of OFF phase in minutes.
   * @param start True to begin toggling; false to stop.
   */
  void setToggle(uint16_t onMinutes, uint16_t offMinutes, bool start)
  {
    bool nextActive = start && (onMinutes + offMinutes > 0);
    if (toggleOnMinutes == onMinutes && toggleOffMinutes == offMinutes && toggleActive == nextActive)
      return;
    Serial.printf("[Relay %u] Toggle mode: %s, ON=%u min, OFF=%u min\n",
                  number, start ? "START" : "STOP", onMinutes, offMinutes);
    toggleOnMinutes = onMinutes;
    toggleOffMinutes = offMinutes;
    toggleActive = nextActive;
    toggleStarted = millis() / 1000;
    if (toggleActive)
      applyState(true);
    save();
  }

  /**
   * Configures the relay for temperature control mode by assigning a DS18B20 sensor address
   * and setting a target temperature. The relay will maintain the target ±TEMPERATURE_HYSTERESIS.
   * Also clears any previous sensor error flag so the next read cycle can recover.
   *
   * @param address Hexadecimal sensor address (e.g., "28AA123456789ABC").
   * @param target Setpoint temperature in Celsius (-55 to +125 for DS18B20).
   */
  void setTemperatureConfig(const String &address, float target)
  {
    String normalizedAddress = address;
    normalizedAddress.toUpperCase();
    if (sensorAddress == normalizedAddress && targetTemperature == target)
      return;
    sensorAddress = normalizedAddress;
    targetTemperature = target;
    sensorErrorReported = false;
    Serial.printf("[Relay %u] Temperature control: sensor=%s, target=%.2f C, hysteresis=+/- %.2f C\n",
                  number, sensorAddress.c_str(), targetTemperature, TEMPERATURE_HYSTERESIS);
    save();
  }

  /**
   * Cancels temperature control mode, clearing all timer and toggle state and forcing the relay OFF.
   * Switches the relay back to manual mode. Used when a temperature sensor is removed or
   * when the user explicitly stops temperature control via the API.
   */
  void stopTemperatureControl()
  {
    bool changed = mode != "manual" || state || toggleActive || timerActive || timerDuration != 0;
    mode = "manual";
    toggleActive = false;
    timerActive = false;
    timerDuration = 0;
    applyState(false);
    if (changed)
      save();
  }

  /**
   * Called when the assigned temperature sensor fails to provide a valid reading.
   * Records the failure, increments the global alarm counter, and switches to manual OFF mode.
   * Generates a display message and sets the global temperature alarm flag to trigger buzzer alerts.
   */
  void handleTemperatureReadFailure()
  {
    if (!temperatureReadFailed)
    {
      temperatureReadFailed = true;
      temperatureReadFailureCount++;
      temperatureReadFailureAlarm = true;
      if (!temperatureReadFailureNames.isEmpty())
        temperatureReadFailureNames += ", ";
      temperatureReadFailureNames += sensorAddress.length() > 4 ? sensorAddress.substring(sensorAddress.length() - 4) : sensorAddress;
      queueDisplayMessage("Temp Sensor Fail\n" + temperatureReadFailureNames);
    }
    currentTemperature = NAN;
    if (mode != "manual" || state)
    {
      mode = "manual";
      toggleActive = false;
      timerActive = false;
      timerDuration = 0;
      applyState(false);
      save();
    }
  }

  /**
   * Marks a sensor failure as resolved when a valid reading is obtained again.
   * Decrements the global alarm counter and clears the alarm flag and failure names
   * when all sensors have recovered.
   */
  void clearTemperatureReadFailure()
  {
    if (!temperatureReadFailed)
      return;
    temperatureReadFailed = false;
    if (temperatureReadFailureCount > 0)
      temperatureReadFailureCount--;
    temperatureReadFailureAlarm = temperatureReadFailureCount > 0;
    if (!temperatureReadFailureAlarm)
      temperatureReadFailureNames = "";
  }

  /**
   * Reads the current temperature from the assigned sensor and updates the relay output state
   * based on the target temperature and hysteresis. Called periodically by loop2 when the relay
   * is in temperature control mode.
   *
   * On read failure, invokes handleTemperatureReadFailure() to trigger alarm and safe shutdown.
   * On successful read, applies hysteresis logic: turn ON if temp <= (target - hysteresis),
   * turn OFF if temp >= (target + hysteresis), otherwise maintain current state.
   */
  void updateTemperature()
  {
    if (!useTempSensor || mode != "temperature" || !enabled || sensorAddress.isEmpty())
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
        addBufferError("DS18B20: Relay " + String(number) + " sensor not found.");
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
        addBufferError("DS18B20: Relay " + String(number) + " sensor read failed.");
        Serial.printf("[Relay %u] ERROR: DS18B20 temperature read failed\n", number);
        sensorErrorReported = true;
      }
      handleTemperatureReadFailure();
      return;
    }
    clearTemperatureReadFailure();
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

  /**
   * Evaluates whether the relay should be ON according to its configured auto-schedule.
   * If the RTC is not available, returns the current state (no change).
   * Handles wrap-around schedules (e.g., on at 22:00, off at 06:00 the next day).
   *
   * @return True if current time falls within the on-time to off-time range; false otherwise.
   */
  bool shouldBeOnNow()
  {
    if (!rtcReady)
      return state;
    DateTime now = rtc.now();
    uint16_t current = now.hour() * 100 + now.minute();
    return offTime > onTime ? current >= onTime && current < offTime
                            : current >= onTime || current < offTime;
  }

  /**
   * Updates internal timer and toggle state machines on every loop2 cycle.
   * Checks for timer expiration and performs the relay toggle action when time runs out.
   * Updates toggle cycle position and applies state changes when the cycle phase boundary is crossed.
   */
  void update()
  {
    if (timerActive && remainingTimer() == 0)
    {
      Serial.printf("[Relay %u] Timer expired, toggling output\n", number);
      timerActive = false;
      toggle();
      timerDuration = 0;
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

/**
 * Factory resets the device by erasing all relay configurations, NVS preferences, and configuration files.
 * Queues a reset message to the display and schedules a device reboot after 5 seconds.
 * This operation is called when the user holds the left button for 10 seconds or via the /api/reset endpoint.
 */
void resetAllSettings()
{
  resetAll = true;
  queueDisplayMessage("Resetting All\nPlease wait");
  for (uint8_t i = 1; i <= NUM_RELAYS; i++)
  {
    String relayFile = "/config/relay" + String(i) + ".json";
    if (LittleFS.exists(relayFile))
      LittleFS.remove(relayFile);
  }

  esp_err_t eraseResult = nvs_flash_erase();
  esp_err_t initResult = nvs_flash_init();
  Serial.printf("[Reset] NVS erase=%s, init=%s\n",
                esp_err_to_name(eraseResult), esp_err_to_name(initResult));

  restartRequested = true;
  restartAt = millis() + 5000;
}

/**
 * Configures Wi-Fi in either station (STA) or access-point (AP) mode based on stored credentials.
 * If no credentials are saved, starts a captive portal AP at IP 192.168.4.1 for Wi-Fi configuration.
 * If credentials are saved, connects to the configured network with a 15-second timeout.
 * On successful connection, the device transitions to normal operation. On failure, stores an error and continues.
 */
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
      bool changed = preferences.getString("ssid", "") != ssid ||
            preferences.getString("password", "") != password;
      bool saved = !changed || preferences.putString("ssid", ssid) > 0;
      saved = !changed || (preferences.putString("password", password) > 0 && saved);
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
      addBufferError("WIFI: connection failed.");
    }
  }
  preferences.end();
}

/**
 * Registers HTTP REST API endpoints for a single relay by its index (1-4).
 * Endpoints include:
 *   - /api/led[N]/name: Get/Set relay name
 *   - /api/led[N]/status: Get relay output state (ON/OFF)
 *   - /api/led[N]/system/state: Get/Set enabled/disabled flag
 *   - /api/led[N]/mode: Get/Set relay mode (manual, auto, timer, toggle, temperature)
 *   - /api/led[N]/toggle: Manual toggle request
 *   - /api/led[N]/schedule: Get/Set on-time and off-time for auto mode
 *   - /api/led[N]/timer: Start/Stop timer in seconds
 *   - /api/led[N]/timer/state: Get active timer state and remaining seconds
 *   - /api/led[N]/temperature: Get/Set temperature control sensor and target
 *   - /api/led[N]/temperature/stop: Stop temperature control mode
 *   - /api/led[N]/schedule/stop: Stop auto schedule mode
 *   - /api/led[N]/toggle-mode: Start/Stop toggle mode with on/off minute intervals
 *   - /api/led[N]/toggle-mode/state: Get toggle cycle state and remaining time
 *
 * @param relayNumber Relay index from 1 to NUM_RELAYS; converted to 0-based array index internally.
 */
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
              doc["mode"] = relays[index]->getMode();
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
  server.on((base + "/temperature/stop").c_str(), HTTP_POST, [index](AsyncWebServerRequest *request)
            {
              relays[index]->stopTemperatureControl();
              request->send(200, "application/json", "{\"success\":true,\"mode\":\"manual\",\"state\":\"OFF\"}"); });
  server.on((base + "/schedule/stop").c_str(), HTTP_POST, [index](AsyncWebServerRequest *request)
            {
              relays[index]->stopTemperatureControl();
              request->send(200, "application/json", "{\"success\":true,\"mode\":\"manual\",\"state\":\"OFF\"}"); });

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
              doc["state"] = relays[index]->getState() ? "ON" : "OFF";
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });

  server.on((base + "/temperature").c_str(), HTTP_GET, [index](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              if (!useTempSensor)
              {
                doc["sensor"] = "";
                doc["targetTemperature"] = 0.0f;
                doc["temperature"] = nullptr;
                request->send(200, "application/json", jsonResponse(doc));
                return;
              }
              doc["sensor"] = relays[index]->getSensorAddress();
              doc["targetTemperature"] = relays[index]->getTargetTemperature();
              doc["temperature"] = relays[index]->getCurrentTemperature();
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on((base + "/temperature").c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
            [index](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              if (!useTempSensor)
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Temperature sensors are disabled\"}");
                return;
              }

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
                addBufferError("DS18B20: Selected sensor was not found.");
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

/**
 * Initializes all HTTP REST API endpoints for the web server.
 * Serves the static web dashboard (index.html) and provides endpoints for:
 *   - System status and version reporting
 *   - Relay control API (delegated to setupRelayApi)
 *   - Sensor readings (temperature list with current values)
 *   - RTC time queries and NTP time synchronization
 *   - Time settings (NTP server and timezone offset)
 *   - System configuration (temperature sensor enable/disable)
 *   - Error buffer retrieval and clearing
 *   - Device reset and reboot endpoints
 * Also registers OTA update handlers (onOTAStart, onOTAProgress, onOTAEnd).
 * Starts the AsyncWebServer after all routes are registered.
 */
void setupServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(LittleFS, wifiSetupMode ? "/wifimanager.html" : "/index.html", "text/html"); });
  server.serveStatic("/", LittleFS, "/");
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "true"); });
  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", SW_VERSION); });
  server.on("/api/system/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["useTempSensor"] = useTempSensor;
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on("/api/system/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument input;
              if (deserializeJson(input, data, length) || !input["useTempSensor"].is<bool>())
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid config\"}");
                return;
              }
              useTempSensor = input["useTempSensor"].as<bool>();
              saveSystemConfig();
              request->send(200, "application/json", "{\"success\":true,\"useTempSensor\":" + String(useTempSensor ? "true" : "false") + "}"); });
  server.on("/api/time-settings", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              JsonArray servers = doc["ntpServers"].to<JsonArray>();
              servers.add("pool.ntp.org");
              servers.add("in.pool.ntp.org");
              servers.add("custom");
              doc["offset"] = timeZoneOffset;
              doc["ntpServer"] = ntpPoolServer;
              doc["customServer"] = customNtpServer;
              String response = jsonResponse(doc);
              request->send(200, "application/json", response); });
  server.on("/api/time-settings", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t length, size_t, size_t)
            {
              JsonDocument input;
                if (deserializeJson(input, data, length) || !input["ntpServer"].is<const char *>() ||
                  !input["offset"].is<long>())
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid time settings\"}");
                return;
              }
              String selectedServer = input["ntpServer"].as<String>();
              String selectedCustomServer = input["customServer"] | "";
              long selectedOffset = input["offset"].as<long>();
              selectedCustomServer.trim();
              String serverName = selectedServer == "custom" ? selectedCustomServer : selectedServer;
              if (selectedOffset < -50400L || selectedOffset > 50400L || serverName.isEmpty())
              {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid timezone or NTP server\"}");
                return;
              }
              timeZoneOffset = selectedOffset;
              ntpPoolServer = selectedServer;
              customNtpServer = selectedCustomServer;
              timeClient.setPoolServerName(serverName.c_str());
              timeClient.setTimeOffset(timeZoneOffset);
              saveTimeSettings();
              bool updated = autoTimeUpdate();
              JsonDocument response;
              response["success"] = true;
              response["rtcUpdated"] = updated;
              response["offset"] = timeZoneOffset;
              response["message"] = updated ? "Time settings saved and RTC updated." : "Time settings saved, but RTC update failed.";
              String responseText = jsonResponse(response);
              request->send(200, "application/json", responseText); });
  server.on("/api/relay-count", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", String(NUM_RELAYS)); });
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
              if (!rtcReady)
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
              resetAllSettings();
              request->send(200, "application/json", "{\"success\":true,\"info\":\"Reset complete. Device will reboot in 5 seconds.\"}"); });
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              Serial.println("[API] /api/reboot requested");
              request->send(200, "application/json", "{\"success\":true,\"info\":\"Device will reboot in 3 seconds.\"}");
              restartRequested = true;
              queueDisplayMessage("Restarting...\nPlease wait");
              restartAt = millis() + 3000; });
  server.on("/api/error", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              JsonDocument doc;
              doc["error"] = errorBuffer;
              String response = jsonResponse(doc);
              request->send(200, "application/json", response);
              errorBuffer = ""; });
  for (uint8_t i = 1; i <= NUM_RELAYS; i++)
    setupRelayApi(i);
  Serial.println("Server setup complete");
  server.begin();
}

/** Task handle for the asynchronous loop2 task running on core 0. */
TaskHandle_t loop2Code;

/**
 * Initializes all hardware peripherals, sensors, storage, and network services.
 * Execution order:
 *   1. Serial communication and console logging
 *   2. GPIO setup for buzzer, buttons, and status LED
 *   3. OLED display initialization with fallback warning
 *   4. LittleFS file system for configuration storage
 *   5. Load persisted system and time settings
 *   6. DS18B20 temperature sensor bus enumeration
 *   7. Relay object construction and configuration loading
 *   8. Wi-Fi setup (AP mode for config, or STA mode for normal operation)
 *   9. RTC clock module check and optional NTP time sync
 *  10. Web server and OTA routes registration (if not in Wi-Fi setup mode)
 *  11. Spawn loop2 task on core 0 for asynchronous polling
 *  12. Signal startup complete with LED flash and buzzer beep
 */
void setup(void)
{
  Serial.begin(115200);
  Serial.println("Starting Smart Aquarium V4.0");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_LEFT, INPUT);
  pinMode(BUTTON_RIGHT, INPUT);
  digitalWrite(BUZZER_PIN, LOW);
  statusLed.begin();
  statusLed.setBrightness(100);
  ledBlink("RED", 1);
  beep(1, 200);

  lastButtonPressTime = millis();
  displayReady = display.begin(i2c_Address, true);
  if (!displayReady)
  {
    Serial.println("[DISPLAY] OLED init failed");
    displayIsOn = false;
    addBufferError("DISPLAY: init failed.");
    beep(4, 200);
  }
  else
  {
    // displayPower(true);
    display.setContrast(0);
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setFont(NULL);
    display.setCursor(7, 10);
    display.println("Smart Aquarium V4.0");
    display.setCursor(28, 35);
    display.println("Initialising");
    display.display();
    delay(500);
  }

  Serial.println("Initializing LittleFS");
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed");
    addBufferError("LFS: mount failed. Config may not save.");
    queueDisplayMessage("LFS mount fail");
  }
  if (!LittleFS.exists("/config"))
    LittleFS.mkdir("/config");

  loadTimeSettings();
  loadSystemConfig();

  if (useTempSensor)
  {
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
      addBufferError("DS18B20: No temperature sensors found " + String(ONE_WIRE_BUS) + ".");
      queueDisplayMessage("DS18B20 Fail");
      Serial.println("[DS18B20] ERROR: No sensors found");
    }
    else
    {
      Serial.printf("[DS18B20] %u sensor(s) ready\n", sensorCount);
    }
  }
  else
  {
    sensorCount = 0;
    Serial.println("[DS18B20] Temperature sensors disabled by configuration");
  }

  Serial.println("Initializing relays");
  for (uint8_t i = 0; i < NUM_RELAYS; i++)
    relays[i] = new Relay(RELAY_PINS[i], i + 1);

  setupWifi();

  Serial.println("Initializing RTC");
  rtcReady = rtc.begin();
  if (!rtcReady)
  {
    Serial.println("RTC not found");
    addBufferError("RTC: not found. Time functions unavailable.");
    queueDisplayMessage("RTC Fail");
  }
  else
  {
    Serial.println("RTC initialized successfully");
    DateTime now = rtc.now();
    Serial.printf("RTC time: %02d:%02d:%02d %02d/%02d/%04d\n",
                  now.hour(), now.minute(), now.second(),
                  now.day(), now.month(), now.year());
  }
  Serial.println();

  if (!wifiSetupMode)
  {
    Serial.println("Setting up server");
    ElegantOTA.begin(&server);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);
    setupServer();
    Serial.println("Server setup complete");
    if (rtcReady)
    {
      DateTime now = rtc.now();
      if (rtc.lostPower() || now.year() < 2026)
      {
        Serial.println("[RTC] RTC lost power or time is invalid, updating from NTP");
        autoTimeUpdate();
      }
    }
  }

  // task executed in the loop2() function, with priority 1 and executed on core 0
  Serial.println("Creating loop2 task");
  xTaskCreatePinnedToCore(
      loop2,       // Task function.
      "loop2Code", // name of task.
      10000,       // Stack size of task
      NULL,        // parameter of the task
      2,           // priority of the task
      &loop2Code,  // Task handle to keep track of created task
      0);          // pin task to core 0
  Serial.println("Setup complete");
  ledBlink("GREEN", 1);
  beep(1, 200);
}

/**
 * Second-priority task running on core 0, handling time-sensitive device polling.
 * Executes continuously in a loop with the following responsibilities:
 *   1. Button input debouncing and long-press detection for factory reset
 *   2. Update relay state machines (timer countdown, toggle cycles)
 *   3. Poll I2C bus health for RTC and OLED devices every 5 seconds
 *   4. Poll temperature sensors and evaluate relay auto-schedule/temperature modes every 2 seconds
 *   5. Yield CPU to other tasks via vTaskDelay(10 ms)
 *
 * The task integrates all stateful control logic and runs asynchronously from loop(),
 * which handles UI rendering and OTA updates. This separation ensures responsive relay control
 * even when the display or network is busy.
 */
void loop2(void *pvParameters)
{
  unsigned long lastScheduleCheck = 0;
  unsigned long lastRtcHealthCheck = 0;
  unsigned long lastButtonCheck = 0;
  unsigned long leftButtonDownSince = 0;
  bool leftButtonWasDown = false;

  for (;;)
  {
    unsigned long currentMillis = millis();
    if (currentMillis - lastButtonCheck >= buttonDebounceMs)
    {
      lastButtonCheck = currentMillis;

      bool leftDown = digitalRead(BUTTON_LEFT) == HIGH;
      bool rightDown = digitalRead(BUTTON_RIGHT) == HIGH;

      if (leftDown)
      {
        if (!leftButtonWasDown)
        {
          leftButtonWasDown = true;
          leftButtonDownSince = currentMillis;
        }
        else if ((currentMillis - leftButtonDownSince) >= 10000UL)
        {
          Serial.println("[Button] LEFT held 10s -> reset all settings");
          resetAllSettings();
          leftButtonDownSince = currentMillis;
        }
      }
      else
      {
        leftButtonWasDown = false;
      }

      if (leftDown || rightDown)
      {
        Serial.println("[Button] pressed");
        lastButtonPressTime = currentMillis;
        markDisplayActivity();
      }
    }

    // Keep timer and toggle modes responsive, independent of schedule polling.
    for (uint8_t i = 0; i < NUM_RELAYS; i++)
      relays[i]->update();

    // Check the RTC health at most once every five seconds.
    if (currentMillis - lastRtcHealthCheck >= 5000UL)
    {
      lastRtcHealthCheck = currentMillis;
      checkI2CHealth();
    }

    // Check auto relay states and the RTC at most once every two seconds.

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

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/**
 * Primary loop running on the main core, handling UI rendering, OTA updates, and time-based actions.
 * Responsibilities:
 *   1. Process ElegantOTA firmware update transfers and flash writes
 *   2. Emit periodic buzzer alarm for active temperature sensor failures (every 3 seconds)
 *   3. Render OLED status screen and handle display message queue:
 *      - If a message is active, show it and wait for button dismissal
 *      - If no message and display is idle, power off after 30 seconds of inactivity
 *      - Otherwise, refresh the status screen every 4 seconds showing Wi-Fi status and IP
 *   4. Perform hourly RTC time validation and automatic NTP sync if time drifted
 *   5. Execute pending device restart when the scheduled restart timestamp is reached
 *   6. Yield CPU to other tasks via vTaskDelay(10 ms)
 *
 * This loop prioritizes UI responsiveness and OTA robustness over polling speed,
 * allowing loop2() to handle rapid relay control independently.
 */
void loop(void)
{
  ElegantOTA.loop();
  unsigned long currentMillis = millis();
  static unsigned long lastTemperatureFailureBeep = 0;
  if (temperatureReadFailureAlarm && currentMillis - lastTemperatureFailureBeep >= 3000UL)
  {
    lastTemperatureFailureBeep = currentMillis;
    beep(3, 200);
  }
  if (displayReady)
  {
    if (displayMessageActive)
    {
      markDisplayActivity();
      bool flag = false;
      while (digitalRead(BUTTON_LEFT) == HIGH || digitalRead(BUTTON_RIGHT) == HIGH)
      {
        yield();
        flag = true;
      }
      if (flag)
      {
        dismissDisplayMessage();
        flag = false;
      }
      drawStatusScreen();
    }
    else if (!displayMessageActive && displayIsOn && (currentMillis - lastButtonPressTime >= DISPLAY_INACTIVITY_TIMEOUT))
    {
      displayIsOn = false;
      displayPower(false);
    }
    else
    {
      static unsigned long lastStatusDraw = 0;
      if (displayIsOn && (displayMessageActive || (currentMillis - lastStatusDraw >= 4000UL)))
      {
        markDisplayActivity();
        lastStatusDraw = currentMillis;
        drawStatusScreen();
      }
    }
  }

  static unsigned long lastTimeCheck = 0;
  if (currentMillis - lastTimeCheck >= 3600000UL)
  {
    lastTimeCheck = currentMillis;
    if (rtcReady)
    {
      DateTime now = rtc.now();
      preferences.begin("time", false);
      uint8_t lastUpdateDay = preferences.getUChar("lastUpdateDay", 0);
      preferences.end();
      uint8_t daysSinceUpdate = lastUpdateDay == 0 ? 15 : (now.day() - lastUpdateDay + 31) % 31;
      if (daysSinceUpdate >= 15)
      {
        Serial.printf("[RTC] Automatic update due: %u days since last update\n", daysSinceUpdate);
        autoTimeUpdate();
      }
    }
    else
    {
      Serial.println("[RTC] Skipping automatic update: RTC not ready");
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

  vTaskDelay(pdMS_TO_TICKS(10));
}
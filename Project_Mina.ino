

/*
Sketch uses 1108846 bytes (84%) of program storage space. Maximum is 1310720 bytes.
Global variables use 50536 bytes (15%) of dynamic memory, leaving 277144 bytes for local variables. Maximum is 327680 bytes.
*/
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// NTP
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "RTClib.h"

RTC_DS3231 rtc; // Initalize rtc
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "asia.pool.ntp.org", 19800); // 19800 is offset of India, asia.pool.ntp.org is close to India 5.5*60*60

// Global preferences instance
Preferences prefs;
/**
 * Software version number
 * Format: major.minor.patch
 */
#define SWVersion "v0.2.0-alpha"

const char *ssid = "SonyBraviaX400";
const char *password = "79756622761";
const int ledPin = 2;

AsyncWebServer server(80);

bool updateTime = false; // Flag to trigger time update

unsigned long timerStarter = 0;
int timerDuration = 0;

class Relay
{
private:
  static Preferences &preferences; // Change to static reference
  bool isOn;
  int onTime;
  int offTime;
  String mode; // "manual", "auto", or "timer"
  bool isDisabled;
  byte pin;
  String name;
  int relayNum;             // Added to track relay number
  unsigned long timerStart; // When timer was started
  int timerDuration;        // Duration in seconds
  bool timerActive;         // If timer is currently running

  /**
   * @brief Loads relay settings from persistent storage
   *
   * Retrieves all saved settings including state, name, timings, and mode
   * from the preferences storage using the relay's unique number as key
   */
  void loadFromPreferences()
  {
    String key = String(relayNum); // Use simple numeric key
    Serial.print("Loading preferences for relay ");
    Serial.println(key);

    isOn = preferences.getBool((key + "_on").c_str(), false);
    isDisabled = preferences.getBool((key + "_disabled").c_str(), false);
    name = preferences.getString((key + "_name").c_str(), "Relay " + key);
    onTime = preferences.getInt((key + "_onTime").c_str(), 0);
    offTime = preferences.getInt((key + "_offTime").c_str(), 0);
    mode = preferences.getString((key + "_mode").c_str(), "manual");
    timerDuration = preferences.getInt((key + "_timerDuration").c_str(), 0);
    timerActive = preferences.getBool((key + "_timerActive").c_str(), false);
    timerStart = preferences.getULong((key + "_timerStart").c_str(), 0);

    Serial.printf("Loaded: on=%d, disabled=%d, name=%s, onTime=%d, offTime=%d, mode=%s\n",
                  isOn, isDisabled, name.c_str(), onTime, offTime, mode.c_str());
    Serial.printf("Loaded timer: duration=%d, active=%d, start=%lu\n",
                  timerDuration, timerActive, timerStart);
  }

  /**
   * @brief Saves current relay settings to persistent storage
   *
   * Stores all current settings including state, name, timings, and mode
   * to the preferences storage using the relay's unique number as key
   */
  void saveToPreferences()
  {
    String key = String(relayNum);
    Serial.print("Saving preferences for relay ");
    Serial.println(key);

    preferences.putBool((key + "_on").c_str(), isOn);
    preferences.putBool((key + "_disabled").c_str(), isDisabled);
    preferences.putString((key + "_name").c_str(), name);
    preferences.putInt((key + "_onTime").c_str(), onTime);
    preferences.putInt((key + "_offTime").c_str(), offTime);
    preferences.putString((key + "_mode").c_str(), mode);
    preferences.putInt((key + "_timerDuration").c_str(), timerDuration);
    preferences.putBool((key + "_timerActive").c_str(), timerActive);
    preferences.putULong((key + "_timerStart").c_str(), timerStart);

    Serial.printf("Saved: on=%d, disabled=%d, name=%s, onTime=%d, offTime=%d, mode=%s\n",
                  isOn, isDisabled, name.c_str(), onTime, offTime, mode.c_str());
    Serial.printf("Saved timer: duration=%d, active=%d, start=%lu\n",
                  timerDuration, timerActive, timerStart);
  }

public:
  /**
   * @brief Constructs a new Relay object
   *
   * @param pinNumber GPIO pin number to control the relay
   * @param num Unique identifier for the relay (1-4)
   */
  Relay(byte pinNumber, int num) : pin(pinNumber), relayNum(num),
                                   timerStart(0), timerDuration(0), timerActive(false) // Simplified constructor
  {
    pinMode(pin, OUTPUT);
    loadFromPreferences();
    digitalWrite(pin, isOn ? HIGH : LOW);
  }

  /**
   * @brief Toggles the relay state if enabled
   *
   * Switches the relay between ON and OFF states
   * Only works if the relay is not disabled
   * Saves the new state to preferences
   */
  void toggle()
  {
    if (!isDisabled)
    {
      isOn = !isOn;
      digitalWrite(pin, isOn ? HIGH : LOW);
      saveToPreferences();
    }
  }

  /**
   * @brief Sets the schedule times for auto mode
   *
   * @param on Time to turn on (HHMM format, e.g. 1430 for 2:30 PM)
   * @param off Time to turn off (HHMM format)
   */
  void setTimes(int on, int off)
  {
    onTime = on;
    offTime = off;
    saveToPreferences();
  }

  /**
   * @brief Sets the operating mode of the relay
   *
   * @param newMode Operating mode ("manual", "auto", or "timer")
   */
  void setMode(String newMode)
  {
    mode = newMode;
    saveToPreferences();
  }

  /**
   * @brief Sets the disabled state of the relay
   *
   * @param disabled true to disable the relay, false to enable
   * When disabled, turns off the relay and prevents state changes
   */
  void setDisabled(bool disabled)
  {
    isDisabled = disabled;
    if (disabled)
    {
      isOn = false;
      digitalWrite(pin, LOW);
    }
    saveToPreferences();
  }

  /**
   * @brief Gets the current state of the relay
   * @return true if relay is ON, false if OFF
   */
  bool getState() { return isOn; }

  /**
   * @brief Checks if the relay is enabled
   * @return true if relay is enabled, false if disabled
   */
  bool isEnabled() { return !isDisabled; }

  /**
   * @brief Gets the current operating mode
   * @return String containing mode ("manual", "auto", or "timer")
   */
  String getMode() { return mode; }

  /**
   * @brief Gets the scheduled ON time
   * @return Integer in HHMM format (e.g. 1430 for 2:30 PM)
   */
  int getOnTime() { return onTime; }

  /**
   * @brief Gets the scheduled OFF time
   * @return Integer in HHMM format (e.g. 1430 for 2:30 PM)
   */
  int getOffTime() { return offTime; }

  /**
   * @brief Gets the relay's name
   * @return String containing the custom name of the relay
   */
  String getName() { return name; }

  /**
   * @brief Sets a custom name for the relay
   * @param newName String containing the new name
   */
  void setName(String newName)
  {
    name = newName;
    saveToPreferences();
  }

  /**
   * @brief Configures and activates/deactivates the timer
   *
   * @param duration Duration in seconds for the timer
   * @param start true to start timer, false to stop
   */
  void setTimer(int duration, bool start)
  {
    timerDuration = duration;
    timerActive = start;
    if (start)
    {
      timerStart = millis();
    }
    saveToPreferences(); // Save timer state to persistent memory
  }

  /**
   * @brief Gets the current timer duration
   * @return Integer duration in seconds
   */
  int getTimerDuration() { return timerDuration; }

  /**
   * @brief Checks if timer is currently running
   * @return true if timer is active, false otherwise
   */
  bool isTimerActive() { return timerActive; }

  /**
   * @brief Gets the timestamp when timer was started
   * @return unsigned long containing the millis() value when timer started
   */
  unsigned long getTimerStart() { return timerStart; }
};

// Initialize preferences once
Preferences gPrefs;
Preferences &Relay::preferences = gPrefs;

const byte RELAY_PINS[] = {26, 27, 14, 12};
const byte NUM_RELAYS = 4;
Relay *relays[NUM_RELAYS]; // Array of pointers

// for creating task attached to CORE 0 of CPU
TaskHandle_t loop1Task;
unsigned long previousMillis = 0;
const long interval = 1000; // 1 seconds interval

/**
 * @brief Updates RTC time from NTP server
 * It fetches the current time from an NTP server and updates the RTC.
 *
 * @return bool Returns true if the time was successfully updated, false otherwise
 * @note Requires an active WiFi connection to function
 */
bool autoTimeUpdate()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    timeClient.begin();
    if (timeClient.update() && timeClient.isTimeSet())
    {
      time_t rawtime = timeClient.getEpochTime();
      struct tm *ti = localtime(&rawtime);

      uint16_t year = ti->tm_year + 1900;
      uint8_t month = ti->tm_mon + 1;
      uint8_t day = ti->tm_mday;

      rtc.adjust(DateTime(year, month, day,
                          timeClient.getHours(),
                          timeClient.getMinutes(),
                          timeClient.getSeconds()));

      Serial.println("RTC updated: " + String(year) + "-" +
                     String(month) + "-" + String(day));
      return true;
    }
    else
      return false;
  }
  else
    return false;
}

void setup()
{
  Wire.begin();
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);

  if (!LittleFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi..");
  }
  Serial.println(WiFi.localIP());

  if (!rtc.begin())
    Serial.println("Couldn't find RTC");

  Serial.println("RTC Ready");

  // Open preferences at start and keep open
  if (!gPrefs.begin("relay_store", false))
  {
    Serial.println("Failed to initialize preferences");
    return;
  }

  // Create relay objects with index numbers
  for (byte i = 0; i < NUM_RELAYS; i++)
  {
    relays[i] = new Relay(RELAY_PINS[i], i + 1);
  }

  // Serve static files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  // this is like a ping, checks if the server is alive
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "true"); });
  // return program version
  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", SWVersion); });

  // Server endpoints for RELAY names
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/name";
    byte ledIndex = i;

    // GET endpoint for retrieval of names
    server.on(endpoint.c_str(), HTTP_GET, [ledIndex](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", relays[ledIndex - 1]->getName()); });

    // POST endpoint for setting name on SETTINGS page
    server.on(endpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, // Empty handler for POST request
              NULL, [ledIndex](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        String json = String((char*)data);
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, json);
        
        if (error) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }

        if (!doc["name"].is<const char*>()) {
          request->send(400, "text/plain", "Missing name field");
          return;
        }

        String newName = doc["name"].as<String>();
        if (newName.length() == 0) {
          request->send(400, "text/plain", "Name cannot be empty");
          return;
        }

        relays[ledIndex-1]->setName(newName);
        request->send(200, "text/plain", "Name updated"); });
  }

  // Server endpoints for RELAY states (ENABLED OR DISABLED)
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/system/state";
    byte ledIndex = i;

    // GET handler for sending state
    server.on(endpoint.c_str(), HTTP_GET, [ledIndex](AsyncWebServerRequest *request)
              {
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      response->print("{\"enabled\": " + String(relays[ledIndex-1]->isEnabled() ? "true" : "false") + "}");
      request->send(response); });

    // POST handler for setting state
    server.on(endpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, // Empty handler required
              NULL, [ledIndex](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }

        if (!doc["enabled"].is<bool>()) {
          request->send(400, "text/plain", "Missing enabled field");
          return;
        }

        bool enabled = doc["enabled"].as<bool>();
        relays[ledIndex-1]->setDisabled(!enabled);
        request->send(200, "text/plain", "State updated"); });
  }

  // Server endpoints for RELAY ON/OFF status
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/status";
    byte ledIndex = i;
    server.on(endpoint.c_str(), HTTP_GET, [ledIndex](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", relays[ledIndex - 1]->getState() ? "ON" : "OFF"); });

    // mode endpoint
    String modeEndpoint = "/api/led" + String(i) + "/mode";
    byte modeIndex = i;

    // GET handler for sending mode
    server.on(modeEndpoint.c_str(), HTTP_GET, [modeIndex](AsyncWebServerRequest *request)
              { request->send(200, "text/plain", relays[modeIndex - 1]->getMode()); });

    // POST handler for retrieving mode
    server.on(modeEndpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [modeIndex](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
        }

        if (!doc["mode"].is<const char*>()) {
            request->send(400, "text/plain", "Missing mode field");
            return;
        }

        String newMode = doc["mode"].as<String>();
        // Validate mode
        if (newMode != "manual" && newMode != "auto" && newMode != "timer") {
            request->send(400, "text/plain", "Invalid mode value");
            return;
        }

        relays[modeIndex-1]->setMode(newMode);
        request->send(200, "text/plain", "Mode updated"); });

    // timer endpoint
    String timerEndpoint = "/api/led" + String(i) + "/timer";
    server.on(timerEndpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [modeIndex](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
        }

        if (!doc["duration"].is<int>() || !doc["state"].is<bool>()) {
            request->send(400, "text/plain", "Missing duration or state field");
            return;
        }

        int duration = doc["duration"].as<int>();
        bool state = doc["state"].as<bool>();
        
        if (!state) {
            // Deactivate timer
            relays[modeIndex-1]->setTimer(0, false);
            request->send(200, "text/plain", "Timer stopped");
            return;
        }
        
        if (duration <= 0) {
            request->send(400, "text/plain", "Invalid duration");
            return;
        }

        // Set timer with provided duration
        relays[modeIndex-1]->setTimer(duration, true);
        request->send(200, "text/plain", "Timer started"); });

    // schedule endpoint for setting on/off times
    String scheduleEndpoint = "/api/led" + String(i) + "/schedule";
    server.on(scheduleEndpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [modeIndex](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
        }

        if (!doc["onTime"].is<const char*>() || !doc["offTime"].is<const char*>()) {
            request->send(400, "text/plain", "Missing time fields");
            return;
        }

        String onTimeStr = doc["onTime"].as<String>();
        String offTimeStr = doc["offTime"].as<String>();
        
        // Convert time strings to integers (e.g., "14:30" -> 1430)
        int onTime = (onTimeStr.substring(0,2).toInt() * 100) + onTimeStr.substring(3,5).toInt();
        int offTime = (offTimeStr.substring(0,2).toInt() * 100) + offTimeStr.substring(3,5).toInt();
        
        if (onTime < 0 || onTime > 2359 || offTime < 0 || offTime > 2359) {
            request->send(400, "text/plain", "Invalid time values");
            return;
        }

        relays[modeIndex-1]->setTimes(onTime, offTime);
        request->send(200, "text/plain", "Schedule updated"); });

    // schedule GET endpoint for sending on/off times
    String scheduleGetEndpoint = "/api/led" + String(i) + "/schedule";
    server.on(scheduleGetEndpoint.c_str(), HTTP_GET, [modeIndex](AsyncWebServerRequest *request)
              {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        response->print("{\"onTime\": " + String(relays[modeIndex-1]->getOnTime()) + 
                      ", \"offTime\": " + String(relays[modeIndex-1]->getOffTime()) + "}");
        request->send(response); });
  }

  // Manual Mode Toggle endpoints for each relay
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/toggle";
    byte ledIndex = i;
    server.on(endpoint.c_str(), HTTP_POST, [ledIndex](AsyncWebServerRequest *request)
              {
      relays[ledIndex-1]->toggle();
      request->send(200, "text/plain", "Toggled"); });
  }

  // This endpoint is used to trigger a time update
  server.on("/api/time/update", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    updateTime = true;
    Serial.println("Time update requested");
    request->send(200, "text/plain", "Time update scheduled"); });

  server.begin();
  xTaskCreatePinnedToCore(
      loop1,       /* Task function. */
      "loop1Task", /* name of task. */
      10000,       /* Stack size of task */
      NULL,        /* parameter of the task */
      1,           /* priority of the task */
      &loop1Task,  /* Task handle to keep track of created task */
      0);          /* pin task to core 0 */
}

void loop()
{
}

void loop1(void *pvParameters)
{
  for (;;)
  {
    byte relayCount = 0;
    unsigned long currentMillis = millis();

    if (updateTime) // automatically updates time when true
    {
      if (autoTimeUpdate())
      {
        Serial.println("Time updated successfully");
        updateTime = false;
      }
      else
        Serial.println("Failed to update time");
    }

    // Check TIMER-MODE for all relays
    for (byte i = 0; i < NUM_RELAYS; i++)
    {
      if (relays[i]->isEnabled() && relays[i]->getMode() == "timer" && relays[i]->isTimerActive())
      {
        unsigned long elapsedTime = (currentMillis - relays[i]->getTimerStart()) / 1000; // Convert to seconds

        if (elapsedTime >= relays[i]->getTimerDuration())
        {
          relays[i]->toggle();           // Toggle state after duration expires
          relays[i]->setTimer(0, false); // Deactivate timer
          relays[i]->setMode("manual");  // Return to manual mode
          Serial.printf("Timer completed for relay %d\n", i + 1);
        }
      }
    }

    // Checking for AUTO-MODE schedule every second
    if (currentMillis - previousMillis >= interval)
    {
      previousMillis = currentMillis;
      for (byte i = 0; i < NUM_RELAYS; i++)
      {
        if (relays[i]->isEnabled())
        {
          // Check if the relay is in auto mode and turn it on/off based on the schedule
          if (relays[i]->getMode() == "auto")
          {
            bool shouldBeOn = turnOnRelay(relays[i]->getOnTime(), relays[i]->getOffTime());
            if (shouldBeOn != relays[i]->getState())
            {
              relays[i]->toggle(); // Toggle only if current state differs from desired state
            }
          }
        }
      }
    }
    delay(50);
  }
}

/**
 * @brief Determines if relay should be on based on current time and schedule
 *
 * Checks if the current time falls within the scheduled on/off period.
 * Handles both same-day schedules (e.g., ON: 09:00, OFF: 17:00) and
 * overnight schedules (e.g., ON: 22:00, OFF: 06:00).
 *
 * @param onTime Time to turn on in HHMM format (e.g., 1430 for 2:30 PM)
 * @param offTime Time to turn off in HHMM format
 * @return true if current time is within the ON period
 * @return false if current time is within the OFF period
 *
 * @note For overnight schedules (offTime < onTime), returns true if
 *       current time is after onTime OR before offTime
 */
bool turnOnRelay(int onTime, int offTime)
{
  DateTime now = rtc.now();
  byte h = now.hour();
  byte m = now.minute();
  int timeString = h * 100 + m;

  if (offTime > onTime)
  {
    return timeString >= onTime && timeString < offTime;
  }
  // Handle overnight case (e.g., ON: 22:00, OFF: 06:00)
  return timeString >= onTime || timeString < offTime;
}

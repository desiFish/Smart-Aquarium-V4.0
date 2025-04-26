#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// Global preferences instance
Preferences prefs;

#define SWVersion "v0.1.0-alpha"

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

    Serial.printf("Loaded: on=%d, disabled=%d, name=%s, onTime=%d, offTime=%d, mode=%s\n",
                  isOn, isDisabled, name.c_str(), onTime, offTime, mode.c_str());
  }

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

    Serial.printf("Saved: on=%d, disabled=%d, name=%s, onTime=%d, offTime=%d, mode=%s\n",
                  isOn, isDisabled, name.c_str(), onTime, offTime, mode.c_str());
  }

public:
  Relay(byte pinNumber, int num) : pin(pinNumber), relayNum(num),
                                   timerStart(0), timerDuration(0), timerActive(false) // Simplified constructor
  {
    pinMode(pin, OUTPUT);
    loadFromPreferences();
    digitalWrite(pin, isOn ? HIGH : LOW);
  }

  void toggle()
  {
    if (!isDisabled)
    {
      isOn = !isOn;
      digitalWrite(pin, isOn ? HIGH : LOW);
      saveToPreferences();
    }
  }

  void setTimes(int on, int off)
  {
    onTime = on;
    offTime = off;
    saveToPreferences();
  }

  void setMode(String newMode)
  {
    mode = newMode;
    saveToPreferences();
  }

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

  bool getState() { return isOn; }
  bool isEnabled() { return !isDisabled; }
  String getMode() { return mode; }
  int getOnTime() { return onTime; }
  int getOffTime() { return offTime; }
  String getName() { return name; }

  void setName(String newName)
  {
    name = newName;
    saveToPreferences();
  }

  // timer methods
  void setTimer(int duration, bool start)
  {
    timerDuration = duration;
    timerActive = start;
    if (start)
    {
      timerStart = millis();
    }
  }

  int getTimerDuration() { return timerDuration; }
  bool isTimerActive() { return timerActive; }
  unsigned long getTimerStart() { return timerStart; }
};

// Initialize preferences once
Preferences gPrefs;
Preferences &Relay::preferences = gPrefs;

const byte RELAY_PINS[] = {26, 27, 14, 12};
const byte NUM_RELAYS = 4;
Relay *relays[NUM_RELAYS]; // Array of pointers

void setup()
{
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

  // Toggle endpoints for each relay
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
}

void loop()
{
}
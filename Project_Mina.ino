#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

Preferences pref;

#define SWVersion "v0.0.3-alpha"

const char *ssid = "SonyBraviaX400";
const char *password = "79756622761";
const int ledPin = 2;

AsyncWebServer server(80);

unsigned long timerStarter = 0;
int timerDuration = 0;

String getLEDState()
{
  return digitalRead(ledPin) ? "ON" : "OFF";
}

void setup()
{
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);

  pref.begin("storage", false);

  if (!pref.isKey("name1"))
    pref.putString("name1", "Relay1");

  if (!pref.isKey("name2"))
    pref.putString("name2", "Relay2");

  if (!pref.isKey("name3"))
    pref.putString("name3", "Relay3");

  if (!pref.isKey("name4"))
    pref.putString("name4", "Relay4");

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

  // Serve static files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "true"); });
  server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", SWVersion); });

  // Server endpoints for RELAY names
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/name";
    byte ledIndex = i; // Rename to avoid conflict with data index parameter

    // GET endpoint
    server.on(endpoint.c_str(), HTTP_GET, [ledIndex, &pref](AsyncWebServerRequest *request)
              {
      String name = pref.getString(("name" + String(ledIndex)).c_str());
      request->send(200, "text/plain", name); });

    // POST endpoint for setting name on SETTINGS>HTML
    server.on(endpoint.c_str(), HTTP_POST, [](AsyncWebServerRequest *request) {}, // Empty handler for POST request
              NULL, [ledIndex, &pref](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t /*index*/, size_t /*total*/)
              {
        String json = String((char*)data);
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, json);
        
        if (error) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }

        if (!doc.containsKey("name")) {
          request->send(400, "text/plain", "Missing name field");
          return;
        }

        String newName = doc["name"].as<String>();
        if (newName.length() == 0) {
          request->send(400, "text/plain", "Name cannot be empty");
          return;
        }

        pref.putString(("name" + String(ledIndex)).c_str(), newName);
        request->send(200, "text/plain", "Name updated"); });
  }

  // Server endpoints for RELAY states (ENABLED OR DISABLED)
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/system/state";
    server.on(endpoint.c_str(), HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncResponseStream *response = request->beginResponseStream("application/json");
      response->print("{\"enabled\": false}");  // true or false 
      request->send(response); });
  }

  // Server endpoints for RELAY ON/OFF status
  for (byte i = 1; i < 5; i++)
  {
    String endpoint = "/api/led" + String(i) + "/status";
    server.on(endpoint.c_str(), HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(200, "text/plain", "ON"); // Return "ON" or "OFF" based on actual RELAY state
              });
  }

  server.begin();
  // pref.end();
}

void loop()
{
}
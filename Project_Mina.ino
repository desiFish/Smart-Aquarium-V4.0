#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

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

  // API endpoints for LED control
  server.on("/api/led", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", getLEDState()); });

  server.on("/api/led/toggle", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    digitalWrite(ledPin, !digitalRead(ledPin));
    request->send(200, "text/plain", getLEDState()); });

  server.on("/api/led/off", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    digitalWrite(ledPin, LOW);
    request->send(200, "text/plain", "OFF"); });

  server.on("/api/timer", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("seconds", true)) {
        String seconds = request->getParam("seconds", true)->value();
        timerDuration = seconds.toInt() * 1000; // Convert to milliseconds
        if (timerDuration > 0) {
            digitalWrite(ledPin, LOW); // Turn off LED when timer starts
            timerStarter = millis();
        } else {
            timerDuration = 0;
        }
        request->send(200, "text/plain", "Timer set");
    } });

  server.begin();
}

void loop()
{
  if (timerDuration > 0)
  {
    if (millis() - timerStarter >= timerDuration)
    {
      digitalWrite(ledPin, HIGH);
      timerDuration = 0; // Reset timer
    }
  }
  // Nothing needed in loop
}
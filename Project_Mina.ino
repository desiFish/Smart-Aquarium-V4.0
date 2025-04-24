#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

const char *ssid = "SonyBraviaX400";
const char *password = "79756622761";

const int ledPin1 = 2; // Built-in LED
const int ledPin2 = 4; // Additional LEDs
const int ledPin3 = 5;
const int ledPin4 = 18;
bool ledState1 = false;
bool ledState2 = false;
bool ledState3 = false;
bool ledState4 = false;
unsigned long previousMillis = 0;
const long blinkInterval = 1000;
bool isBlinking1 = false;
bool isBlinking2 = false;
bool isBlinking3 = false;
bool isBlinking4 = false;

AsyncWebServer server(80);

unsigned long ota_progress_millis = 0;

void onOTAStart()
{
  // Log when OTA has started
  Serial.println("OTA update started!");
  // <Add your own code here>
}

void onOTAProgress(size_t current, size_t final)
{
  // Log every 1 second
  if (millis() - ota_progress_millis > 1000)
  {
    ota_progress_millis = millis();
    Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
  }
}

void onOTAEnd(bool success)
{
  // Log when OTA has finished
  if (success)
  {
    Serial.println("OTA update finished successfully!");
  }
  else
  {
    Serial.println("There was an error during OTA update!");
  }
  // <Add your own code here>
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html {font-family: Arial; display: inline-block; text-align: center;}
    body {
      max-width: 600px; 
      margin:0px auto; 
      padding: 20px;
      background-color: #1a1a1a;
      color: #ffffff;
    }
    .switch {position: relative; display: inline-block; width: 60px; height: 34px; margin: 10px;}
    .switch input {display: none;}
    .slider {
      position: absolute; 
      cursor: pointer; 
      top: 0; 
      left: 0; 
      right: 0; 
      bottom: 0; 
      background-color: #333;
      transition: .4s; 
      border-radius: 34px;
    }
    .slider:before {position: absolute; content: ""; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%;}
    input:checked + .slider {background-color: #2196F3;}
    input:checked + .slider:before {transform: translateX(26px);}
    h2 {color: #2196F3;}
    .led-control {
      margin: 20px;
      background-color: #252525;
      padding: 15px;
      border-radius: 10px;
    }
  </style>
</head>
<body>
  <h2>Smart Aquarium V4</h2>
  <div class="led-control">
    <p>RELAY 1 Control</p>
    <label class="switch">
      <input type="checkbox" onchange="toggleLED(this, 1)">
      <span class="slider"></span>
    </label>
  </div>
  <div class="led-control">
    <p>RELAY 2 Control</p>
    <label class="switch">
      <input type="checkbox" onchange="toggleLED(this, 2)">
      <span class="slider"></span>
    </label>
  </div>
  <div class="led-control">
    <p>RELAY 3 Control</p>
    <label class="switch">
      <input type="checkbox" onchange="toggleLED(this, 3)">
      <span class="slider"></span>
    </label>
  </div>
  <div class="led-control">
    <p>RELAY 4 Control</p>
    <label class="switch">
      <input type="checkbox" onchange="toggleLED(this, 4)">
      <span class="slider"></span>
    </label>
  </div>
  <script>
    function toggleLED(element, led) {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", `/toggle?led=${led}&state=${element.checked ? "1" : "0"}`, true);
      xhr.send();
    }
  </script>
</body>
</html>
)rawliteral";

void setup(void)
{
  Serial.begin(115200);
  pinMode(ledPin1, OUTPUT);
  pinMode(ledPin2, OUTPUT);
  pinMode(ledPin3, OUTPUT);
  pinMode(ledPin4, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send_P(200, "text/html", index_html); });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              if (request->hasParam("led") && request->hasParam("state"))
              {
                int led = request->getParam("led")->value().toInt();
                String state = request->getParam("state")->value();
                switch(led) {
                  case 1: isBlinking1 = (state == "1"); break;
                  case 2: isBlinking2 = (state == "1"); break;
                  case 3: isBlinking3 = (state == "1"); break;
                  case 4: isBlinking4 = (state == "1"); break;
                }
              }
              request->send(200, "text/plain", "OK"); });

  ElegantOTA.begin(&server); // Start ElegantOTA
  // ElegantOTA callbacks
  ElegantOTA.onStart(onOTAStart);
  ElegantOTA.onProgress(onOTAProgress);
  ElegantOTA.onEnd(onOTAEnd);

  server.begin();
  Serial.println("HTTP server started");
}

void loop(void)
{
  ElegantOTA.loop();
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= blinkInterval)
  {
    previousMillis = currentMillis;

    if (isBlinking1)
    {
      ledState1 = !ledState1;
      digitalWrite(ledPin1, ledState1);
    }
    else
    {
      digitalWrite(ledPin1, LOW);
    }

    if (isBlinking2)
    {
      ledState2 = !ledState2;
      digitalWrite(ledPin2, ledState2);
    }
    else
    {
      digitalWrite(ledPin2, LOW);
    }

    if (isBlinking3)
    {
      ledState3 = !ledState3;
      digitalWrite(ledPin3, ledState3);
    }
    else
    {
      digitalWrite(ledPin3, LOW);
    }

    if (isBlinking4)
    {
      ledState4 = !ledState4;
      digitalWrite(ledPin4, ledState4);
    }
    else
    {
      digitalWrite(ledPin4, LOW);
    }
  }
}

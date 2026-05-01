#include <WiFi.h>
#include <WebServer.h>

// ================= WiFi =================
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "99998888";

// ================= Pins =================
#define SOIL_PIN 34   // Analog pin for soil moisture sensor

// ================= Calibration =================
// عدلها حسب حساسك
// جاف = قيمة أعلى غالباً
// مبلل = قيمة أقل غالباً
const int SOIL_DRY_RAW = 3000;
const int SOIL_WET_RAW = 1200;

// أقل نسبة نعتبر بعدها التربة جافة
const int DRY_THRESHOLD_PERCENT = 35;

WebServer server(80);

// ================= Functions =================
int readSoilAverage(int pin, int samples = 15) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(5);
  }
  return sum / samples;
}

int calculateSoilPercent(int raw) {
  int percent = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  percent = constrain(percent, 0, 100);
  return percent;
}

bool getIsDry(int percent) {
  return percent <= DRY_THRESHOLD_PERCENT;
}

// ================= Routes =================
void handleRoot() {
  server.send(200, "text/plain", "Soil Sensor ESP32 is running. Use /read to get JSON reading.");
}

void handleRead() {
  int raw = readSoilAverage(SOIL_PIN);
  int percent = calculateSoilPercent(raw);
  bool isDry = getIsDry(percent);

  String json = "{";
  json += "\"raw\":" + String(raw) + ",";
  json += "\"percent\":" + String(percent) + ",";
  json += "\"isDry\":" + String(isDry ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);

  Serial.print("Raw: ");
  Serial.print(raw);
  Serial.print(" | Percent: ");
  Serial.print(percent);
  Serial.print("% | isDry: ");
  Serial.println(isDry ? "true" : "false");
}

// ================= Setup =================
void setup() {
  Serial.begin(115200);

  pinMode(SOIL_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("Soil Sensor IP: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/read", HTTP_GET, handleRead);

  server.begin();
}

// ================= Loop =================
void loop() {
  server.handleClient();
}

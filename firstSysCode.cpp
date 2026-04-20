#include <WiFi.h>
#include <WebServer.h>

// ===== WiFi =====
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "99998888";

// ===== Soil Sensor Pin =====
#define SOIL_PIN 34   // Analog pin for soil sensor (ADC1)

// ===== Calibration =====
// عدلها حسب حساسك
// SOIL_DRY_RAW  = القراءة لما التربة جافة
// SOIL_WET_RAW  = القراءة لما التربة مبللة
int SOIL_DRY_RAW = 3000;
int SOIL_WET_RAW = 1200;

WebServer server(80);

// ===== Functions =====
int readSoilAverage(int pin, int samples = 15) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(5);
  }
  return sum / samples;
}

int calculateSoilPercent(int raw) {
  // غالبًا حساس الرطوبة يعطي قيمة أكبر لما يكون جاف
  int percent = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  percent = constrain(percent, 0, 100);
  return percent;
}

bool getIsDry(int percent) {
  return percent <= 35;   // عدل الحد إذا احتجت
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

  Serial.println("==== Soil Reading ====");
  Serial.print("Raw: ");
  Serial.println(raw);
  Serial.print("Percent: ");
  Serial.print(percent);
  Serial.println("%");
  Serial.print("isDry: ");
  Serial.println(isDry ? "true" : "false");
}

void handleRoot() {
  String msg = "Soil Sensor ESP32 is running.\nUse /read to get JSON reading.";
  server.send(200, "text/plain", msg);
}

void setup() {
  Serial.begin(115200);
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

void loop() {
  server.handleClient();
}
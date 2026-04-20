#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ================= WiFi =================
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "99998888";

// ===== IP of System 1 (Soil Sensor ESP32) =====
const char* SENSOR_IP = "172.20.10.12";

// ================= Pins =================
#define RELAY_PIN 33
#define WATER_PIN 34

// ================= Settings =================
#define PUMP_DURATION 10000
#define WATER_SAMPLES 31   // odd number for median
#define ADC_SETTLE_DELAY 3

// ================= Water Calibration =================
// Change these after testing
const int WATER_EMPTY_RAW = 1500;
const int WATER_FULL_RAW  = 2155;

// Hysteresis to reduce jumping
const int PERCENT_HYSTERESIS = 3;

// Minimum water percent required to run pump
const int MIN_WATER_PERCENT_TO_RUN = 20;

// ================= Variables =================
WebServer server(80);

bool pumpRunning  = false;
bool autoMode     = true;
bool readingDone  = false;
bool powerEnabled = true;

int soilRaw       = 0;
int soilPercent   = 0;
bool isDry        = false;

int waterRaw      = 0;
int waterPercent  = 0;
int lastWaterPercent = 0;
bool waterOK      = true;

String lastCmd = "None / لا يوجد";
unsigned long pumpStart = 0;

// ================= Helper Functions =================
void sortArray(int arr[], int size) {
  for (int i = 0; i < size - 1; i++) {
    for (int j = i + 1; j < size; j++) {
      if (arr[j] < arr[i]) {
        int t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
      }
    }
  }
}

int readWaterStableRaw(int pin) {
  int samples[WATER_SAMPLES];

  for (int i = 0; i < WATER_SAMPLES; i++) {
    samples[i] = analogRead(pin);
    delay(ADC_SETTLE_DELAY);
  }

  sortArray(samples, WATER_SAMPLES);

  // Take middle section only, discard noisy extremes
  int start = WATER_SAMPLES / 4;
  int end   = WATER_SAMPLES - start;

  long sum = 0;
  int count = 0;

  for (int i = start; i < end; i++) {
    sum += samples[i];
    count++;
  }

  return sum / count;
}

int calculateWaterPercent(int raw) {
  if (raw <= WATER_EMPTY_RAW) return 0;
  if (raw >= WATER_FULL_RAW) return 100;

  float percent = ((float)(raw - WATER_EMPTY_RAW) * 100.0) /
                  ((float)(WATER_FULL_RAW - WATER_EMPTY_RAW));

  return constrain((int)(percent + 0.5), 0, 100);
}

int applyHysteresis(int newPercent, int oldPercent) {
  if (abs(newPercent - oldPercent) <= PERCENT_HYSTERESIS) {
    return oldPercent;
  }
  return newPercent;
}

void readWaterLevel() {
  waterRaw = readWaterStableRaw(WATER_PIN);

  int newPercent = calculateWaterPercent(waterRaw);
  waterPercent = applyHysteresis(newPercent, lastWaterPercent);
  lastWaterPercent = waterPercent;

  waterOK = (waterPercent > MIN_WATER_PERCENT_TO_RUN);

  Serial.print("Water Raw: ");
  Serial.print(waterRaw);
  Serial.print(" | Water Percent: ");
  Serial.print(waterPercent);
  Serial.println("%");
}

// ================= Pump Control =================
void pumpON(String reason) {
  readWaterLevel();

  if (!powerEnabled) {
    lastCmd = "⛔ الكهرباء مقطوعة / Power Disabled";
    Serial.println("Pump blocked: power disabled");
    return;
  }

  if (!waterOK) {
    lastCmd = "⛔ الماء منخفض / Water Low";
    Serial.println("Pump blocked: water level too low");
    return;
  }

  digitalWrite(RELAY_PIN, HIGH);
  pumpRunning = true;
  pumpStart = millis();
  lastCmd = reason;

  Serial.println("Pump ON: " + reason);
}

void pumpOFF(String reason) {
  digitalWrite(RELAY_PIN, LOW);
  pumpRunning = false;
  lastCmd = reason;

  Serial.println("Pump OFF: " + reason);
}

// ================= Soil Sensor Fetch =================
bool fetchSensorReading() {
  HTTPClient http;
  String url = "http://" + String(SENSOR_IP) + "/read";

  Serial.println("Requesting soil sensor from: " + url);

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    Serial.println("Sensor Response: " + body);

    int rawStart = body.indexOf("\"raw\":");
    int percentStart = body.indexOf("\"percent\":");
    int isDryStart = body.indexOf("\"isDry\":");

    if (rawStart == -1 || percentStart == -1 || isDryStart == -1) {
      Serial.println("JSON parse error");
      http.end();
      return false;
    }

    soilRaw = body.substring(rawStart + 6, body.indexOf(",\"percent\"")).toInt();
    soilPercent = body.substring(percentStart + 10, body.indexOf(",\"isDry\"")).toInt();
    isDry = body.indexOf("\"isDry\":true") >= 0;

    readingDone = true;
    http.end();
    return true;
  }

  Serial.print("Failed to fetch soil reading. HTTP code = ");
  Serial.println(code);
  http.end();
  return false;
}

// ================= Build Page =================
String buildPage() {
  readWaterLevel();

  String pumpColor = pumpRunning ? "#27ae60" : "#e74c3c";
  String pumpAR    = pumpRunning ? "شغالة 🟢" : "متوقفة 🔴";
  String pumpEN    = pumpRunning ? "Running" : "Stopped";

  String modeColor = autoMode ? "#3498db" : "#e67e22";
  String modeAR    = autoMode ? "تلقائي 🤖" : "يدوي ✋";
  String modeEN    = autoMode ? "Automatic" : "Manual";

  String powerColor = powerEnabled ? "#27ae60" : "#e74c3c";
  String powerAR    = powerEnabled ? "متصلة ⚡" : "مقطوعة ⛔";
  String powerEN    = powerEnabled ? "Connected" : "Disconnected";
  String powerBtnBG = powerEnabled ? "#e74c3c" : "#27ae60";
  String powerBtnAR = powerEnabled ? "⛔ قطع الكهرباء / Cut Power" : "⚡ وصل الكهرباء / Connect Power";

  String soilColor = "#888";
  String soilAR    = "لم تتم القراءة بعد";
  String soilEN    = "No reading yet";
  String soilAlertBG = "#95a5a6";
  String soilAlertAR = "اضغط زر القراءة";
  String soilAlertEN = "Press Read button";

  if (readingDone) {
    if (isDry) {
      soilColor = "#e74c3c";
      soilAR = "جافة 🔴";
      soilEN = "Dry";
      soilAlertBG = "#e74c3c";
      soilAlertAR = "⚠️ التربة تحتاج ري";
      soilAlertEN = "Soil needs irrigation";
    } else {
      soilColor = "#27ae60";
      soilAR = "مبللة 🟢";
      soilEN = "Wet";
      soilAlertBG = "#27ae60";
      soilAlertAR = "✅ التربة ممتازة";
      soilAlertEN = "Soil is fine";
    }
  }

  String waterColor;
  String waterAR;
  String waterEN;
  String waterAlertBG;
  String waterAlertAR;
  String waterAlertEN;

  if (waterPercent >= 70) {
    waterColor = "#27ae60";
    waterAR = "ممتاز 🟢";
    waterEN = "Good";
    waterAlertBG = "#27ae60";
    waterAlertAR = "✅ مستوى الماء ممتاز";
    waterAlertEN = "Water level is good";
  } else if (waterPercent > MIN_WATER_PERCENT_TO_RUN) {
    waterColor = "#f39c12";
    waterAR = "متوسط 🟠";
    waterEN = "Medium";
    waterAlertBG = "#f39c12";
    waterAlertAR = "⚠️ مستوى الماء متوسط";
    waterAlertEN = "Water level is medium";
  } else {
    waterColor = "#e74c3c";
    waterAR = "فارغ / منخفض ⛔";
    waterEN = "Empty / Low";
    waterAlertBG = "#e74c3c";
    waterAlertAR = "⛔ الماء منخفض - المضخة محجوبة";
    waterAlertEN = "Water level is low - pump blocked";
  }

  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Smart Farm</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: Arial, sans-serif; background: #f0f4f8; padding: 20px; }
    .card {
      background: white;
      border-radius: 16px;
      padding: 24px;
      max-width: 520px;
      margin: 0 auto 20px;
      box-shadow: 0 4px 16px rgba(0,0,0,0.1);
    }
    h1 { text-align: center; color: #2c3e50; margin-bottom: 20px; font-size: 22px; }
    .row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 0;
      border-bottom: 1px solid #eee;
      gap: 12px;
    }
    .label-ar { color: #555; font-size: 15px; }
    .label-en { color: #aaa; font-size: 12px; }
    .value { font-weight: bold; font-size: 16px; text-align: right; }
    .alert-box {
      border-radius: 10px;
      padding: 14px;
      margin: 16px 0;
      text-align: center;
      font-size: 15px;
      font-weight: bold;
      color: white;
    }
    .btn {
      width: 100%;
      padding: 14px;
      font-size: 16px;
      border: none;
      border-radius: 10px;
      cursor: pointer;
      margin: 6px 0;
      font-weight: bold;
    }
    .btn-read    { background: #3498db; color: white; }
    .btn-auto    { background: #3498db; color: white; }
    .btn-manual  { background: #e67e22; color: white; }
    .btn-on      { background: #27ae60; color: white; }
    .btn-off     { background: #e74c3c; color: white; }
    .btn-refresh { background: #8e44ad; color: white; }
    .btn:hover { opacity: 0.9; }
    .divider { border: none; border-top: 2px dashed #ddd; margin: 18px 0; }
    .section-title {
      text-align: center;
      color: #999;
      font-size: 12px;
      margin-bottom: 8px;
      text-transform: uppercase;
    }
    .info-box {
      background: #3498db;
      border-radius: 10px;
      padding: 12px;
      text-align: center;
      color: white;
      font-size: 14px;
      margin: 8px 0;
    }
  </style>
  <script>
    function refreshWater() {
      fetch('/water-status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('waterRaw').innerText = data.raw;
          document.getElementById('waterPercent').innerText = data.percent + "%";

          let statusHTML = "";
          let alertHTML = "";
          let color = "#27ae60";

          if (data.percent >= 70) {
            statusHTML = "<span style='color:#27ae60'>ممتاز 🟢<br><small>Good</small></span>";
            color = "#27ae60";
            alertHTML = "✅ مستوى الماء ممتاز<br><small>Water level is good</small>";
          } else if (data.percent > data.minPercent) {
            statusHTML = "<span style='color:#f39c12'>متوسط 🟠<br><small>Medium</small></span>";
            color = "#f39c12";
            alertHTML = "⚠️ مستوى الماء متوسط<br><small>Water level is medium</small>";
          } else {
            statusHTML = "<span style='color:#e74c3c'>فارغ / منخفض ⛔<br><small>Empty / Low</small></span>";
            color = "#e74c3c";
            alertHTML = "⛔ مستوى الماء منخفض - المضخة محجوبة<br><small>Water level is low - pump blocked</small>";
          }

          document.getElementById('waterVal').innerHTML = statusHTML;
          document.getElementById('waterAlert').style.background = color;
          document.getElementById('waterAlert').innerHTML = alertHTML;
        });
    }
  </script>
</head>
<body>
<div class='card'>
  <h1>💧 Water Tank / خزان الماء</h1>

  <div class='row'>
    <div>
      <div class='label-ar'>القراءة الخام</div>
      <div class='label-en'>Raw Reading</div>
    </div>
    <div class='value' id='waterRaw'>)rawliteral" + String(waterRaw) + R"rawliteral(</div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>نسبة الماء</div>
      <div class='label-en'>Water Percentage</div>
    </div>
    <div class='value' id='waterPercent'>)rawliteral" + String(waterPercent) + R"rawliteral(%</div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>حالة الماء</div>
      <div class='label-en'>Water Status</div>
    </div>
    <div class='value' id='waterVal' style='color:)rawliteral" + waterColor + R"rawliteral('>
      )rawliteral" + waterAR + R"rawliteral(<br>
      <small>)rawliteral" + waterEN + R"rawliteral(</small>
    </div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>الكهرباء</div>
      <div class='label-en'>Power</div>
    </div>
    <div class='value' style='color:)rawliteral" + powerColor + R"rawliteral('>
      )rawliteral" + powerAR + R"rawliteral(<br>
      <small>)rawliteral" + powerEN + R"rawliteral(</small>
    </div>
  </div>

  <div class='alert-box' id='waterAlert' style='background:)rawliteral" + waterAlertBG + R"rawliteral('>
    )rawliteral" + waterAlertAR + R"rawliteral(<br>
    <small>)rawliteral" + waterAlertEN + R"rawliteral(</small>
  </div>

  <button class='btn btn-refresh' onclick='refreshWater()'>🔄 تحديث الماء / Refresh Water</button>

  <form action='/power' method='POST'>
    <button class='btn' style='background:)rawliteral" + powerBtnBG + R"rawliteral(;color:white'>
      )rawliteral" + powerBtnAR + R"rawliteral(
    </button>
  </form>
</div>

<div class='card'>
  <h1>🌱 Smart Farm Dashboard</h1>

  <div class='section-title'>Soil Sensor</div>

  <div class='row'>
    <div>
      <div class='label-ar'>القيمة الخام</div>
      <div class='label-en'>Raw Value</div>
    </div>
    <div class='value'>)rawliteral" + (readingDone ? String(soilRaw) : "--") + R"rawliteral(</div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>نسبة الرطوبة</div>
      <div class='label-en'>Moisture Percentage</div>
    </div>
    <div class='value'>)rawliteral" + (readingDone ? String(soilPercent) + "%" : "--") + R"rawliteral(</div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>حالة التربة</div>
      <div class='label-en'>Soil Status</div>
    </div>
    <div class='value' style='color:)rawliteral" + soilColor + R"rawliteral('>
      )rawliteral" + soilAR + R"rawliteral(<br>
      <small>)rawliteral" + soilEN + R"rawliteral(</small>
    </div>
  </div>

  <div class='alert-box' style='background:)rawliteral" + soilAlertBG + R"rawliteral('>
    )rawliteral" + soilAlertAR + R"rawliteral(<br>
    <small>)rawliteral" + soilAlertEN + R"rawliteral(</small>
  </div>

  <form action='/read' method='POST'>
    <button class='btn btn-read' type='submit'>📊 قراءة حساس الرطوبة / Read Soil Sensor</button>
  </form>

  <hr class='divider'>

  <div class='section-title'>Pump Status</div>

  <div class='row'>
    <div>
      <div class='label-ar'>المضخة</div>
      <div class='label-en'>Pump</div>
    </div>
    <div class='value' style='color:)rawliteral" + pumpColor + R"rawliteral('>
      )rawliteral" + pumpAR + R"rawliteral(<br>
      <small>)rawliteral" + pumpEN + R"rawliteral(</small>
    </div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>الوضع</div>
      <div class='label-en'>Mode</div>
    </div>
    <div class='value' style='color:)rawliteral" + modeColor + R"rawliteral('>
      )rawliteral" + modeAR + R"rawliteral(<br>
      <small>)rawliteral" + modeEN + R"rawliteral(</small>
    </div>
  </div>

  <div class='row'>
    <div>
      <div class='label-ar'>آخر أمر</div>
      <div class='label-en'>Last Command</div>
    </div>
    <div class='value' style='font-size:13px'>)rawliteral" + lastCmd + R"rawliteral(</div>
  </div>

  <hr class='divider'>

  <form action='/mode-auto' method='POST'>
    <button class='btn btn-auto' type='submit'>🤖 Auto Mode</button>
  </form>

  <form action='/mode-manual' method='POST'>
    <button class='btn btn-manual' type='submit'>✋ Manual Mode</button>
  </form>
)rawliteral";

  if (!autoMode) {
    page += R"rawliteral(
  <form action='/on' method='POST'>
    <button class='btn btn-on' type='submit'>🟢 Pump ON</button>
  </form>

  <form action='/off' method='POST'>
    <button class='btn btn-off' type='submit'>🔴 Pump OFF</button>
  </form>
)rawliteral";
  } else {
    page += R"rawliteral(
  <div class='info-box'>
    Auto mode is enabled
  </div>
)rawliteral";
  }

  page += R"rawliteral(
</div>
</body>
</html>
)rawliteral";

  return page;
}

// ================= Routes =================
void handleHome() {
  server.send(200, "text/html", buildPage());
}

void handleWaterStatus() {
  readWaterLevel();

  String json = "{";
  json += "\"raw\":" + String(waterRaw) + ",";
  json += "\"percent\":" + String(waterPercent) + ",";
  json += "\"waterOK\":" + String(waterOK ? "true" : "false") + ",";
  json += "\"minPercent\":" + String(MIN_WATER_PERCENT_TO_RUN);
  json += "}";

  server.send(200, "application/json", json);
}

void handlePowerToggle() {
  powerEnabled = !powerEnabled;

  if (!powerEnabled && pumpRunning) {
    pumpOFF("Power Cut");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRead() {
  bool success = fetchSensorReading();

  if (success && autoMode) {
    readWaterLevel();

    if (isDry && !pumpRunning && waterOK) {
      pumpON("Auto Mode - Soil Dry");
    } else if (!isDry && pumpRunning) {
      pumpOFF("Auto Mode - Soil Wet");
    }
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOn() {
  if (!autoMode) {
    pumpON("Manual ON");
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOff() {
  if (!autoMode) {
    pumpOFF("Manual OFF");
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeAuto() {
  autoMode = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeManual() {
  autoMode = false;

  if (pumpRunning) {
    pumpOFF("Switched to Manual");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

// ================= Setup =================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(WATER_PIN, ADC_11db);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("Main Controller IP: http://");
  Serial.println(WiFi.localIP());

  readWaterLevel();

  Serial.println("=== Water Calibration ===");
  Serial.print("EMPTY RAW = ");
  Serial.println(WATER_EMPTY_RAW);
  Serial.print("FULL RAW  = ");
  Serial.println(WATER_FULL_RAW);

  server.on("/", HTTP_GET, handleHome);
  server.on("/water-status", HTTP_GET, handleWaterStatus);
  server.on("/power", HTTP_POST, handlePowerToggle);
  server.on("/read", HTTP_POST, handleRead);
  server.on("/on", HTTP_POST, handlePumpOn);
  server.on("/off", HTTP_POST, handlePumpOff);
  server.on("/mode-auto", HTTP_POST, handleModeAuto);
  server.on("/mode-manual", HTTP_POST, handleModeManual);

  server.begin();
}

// ================= Loop =================
void loop() {
  server.handleClient();

  if (pumpRunning && (millis() - pumpStart >= PUMP_DURATION)) {
    pumpOFF("Auto Stop - Timer");
  }

  if (pumpRunning) {
    readWaterLevel();

    if (!waterOK) {
      pumpOFF("Water Too Low");
      Serial.println("Pump stopped بسبب انخفاض الماء");
    }
  }
}
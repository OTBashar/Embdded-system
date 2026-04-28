#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ================= WiFi =================
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "99998888";

// ===== IP of System 1 Soil Sensor ESP32 =====
const char* SENSOR_IP = "172.20.10.12";

// ================= Pins =================
#define RELAY_PIN 26
#define WATER_PIN 34
#define RELAY_ACTIVE_LOW true

// ================= Settings =================
#define WATER_SAMPLES 31
#define ADC_SETTLE_DELAY 3

// Auto update intervals
const unsigned long WATER_UPDATE_INTERVAL = 1000;  // 1 second
const unsigned long SOIL_UPDATE_INTERVAL  = 5000;  // 5 seconds
const unsigned long DEBUG_INTERVAL        = 2000;  // 2 seconds

// ================= Water Calibration =================
// Based on your readings:
// empty tank ~= 1000 raw, quarter dipped ~= 1500 raw.
// That predicts full tank around 3000 raw if the sensor response is roughly linear.
const int WATER_EMPTY_RAW = 1343;
const int WATER_FULL_RAW  = 1633;

// ================= Thresholds =================
const int PERCENT_HYSTERESIS = 3;
const int MIN_WATER_PERCENT_TO_RUN = 20;

// ================= Variables =================
WebServer server(80);

bool pumpRunning  = false;
bool autoMode     = true;
bool readingDone  = false;
bool powerEnabled = true;   // No button now, stays enabled

bool overrideMode = false;
int overrideSeconds = 10;
unsigned long overrideStart = 0;
unsigned long overrideDurationMs = 10000;

int soilRaw     = 0;
int soilPercent = 0;
bool isDry      = false;

int waterRaw = 0;
int waterPercent = 0;
int lastWaterPercent = 0;
bool waterOK = true;

String lastCmd = "None / لا يوجد";

unsigned long lastWaterUpdate = 0;
unsigned long lastSoilUpdate  = 0;
unsigned long lastDebug       = 0;

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

void relaySet(bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  } else {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  }
}

bool relayIsOn() {
  int state = digitalRead(RELAY_PIN);
  if (RELAY_ACTIVE_LOW) return state == LOW;
  return state == HIGH;
}

void syncPumpState() {
  pumpRunning = relayIsOn();
}

int readWaterStableRaw(int pin) {
  int samples[WATER_SAMPLES];

  for (int i = 0; i < WATER_SAMPLES; i++) {
    samples[i] = analogRead(pin);
    delay(ADC_SETTLE_DELAY);
  }

  sortArray(samples, WATER_SAMPLES);

  int start = WATER_SAMPLES / 4;
  int end = WATER_SAMPLES - start;

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

  waterOK = (waterPercent >= MIN_WATER_PERCENT_TO_RUN);

  Serial.print("Water Raw: ");
  Serial.print(waterRaw);
  Serial.print(" | Water Percent: ");
  Serial.print(waterPercent);
  Serial.println("%");
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "");
  return s;
}

bool extractIntValue(const String& body, const String& key, int& out) {
  int keyPos = body.indexOf("\"" + key + "\"");
  if (keyPos == -1) return false;

  int colon = body.indexOf(":", keyPos);
  if (colon == -1) return false;

  int pos = colon + 1;
  while (pos < body.length() && body[pos] == ' ') pos++;

  int start = pos;
  if (pos < body.length() && body[pos] == '-') pos++;

  while (pos < body.length() && body[pos] >= '0' && body[pos] <= '9') {
    pos++;
  }

  if (pos == start) return false;

  out = body.substring(start, pos).toInt();
  return true;
}

bool extractBoolValue(const String& body, const String& key, bool& out) {
  int keyPos = body.indexOf("\"" + key + "\"");
  if (keyPos == -1) return false;

  int colon = body.indexOf(":", keyPos);
  if (colon == -1) return false;

  int pos = colon + 1;
  while (pos < body.length() && body[pos] == ' ') pos++;

  if (body.substring(pos, pos + 4) == "true") {
    out = true;
    return true;
  }

  if (body.substring(pos, pos + 5) == "false") {
    out = false;
    return true;
  }

  return false;
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

// ================= Pump Control =================
void forcePumpON(String reason) {
  if (!powerEnabled) {
    lastCmd = "Power Disabled / الكهرباء مقطوعة";
    relaySet(false);
    pumpRunning = false;
    return;
  }

  relaySet(true);
  pumpRunning = true;
  lastCmd = reason;

  Serial.println("Pump FORCE ON: " + reason);
}

void pumpON(String reason) {
  readWaterLevel();

  if (!powerEnabled) {
    lastCmd = "Power Disabled / الكهرباء مقطوعة";
    relaySet(false);
    pumpRunning = false;
    return;
  }

  if (!waterOK) {
    lastCmd = "Water Low / الماء منخفض";
    relaySet(false);
    pumpRunning = false;
    Serial.println("Pump blocked: water level too low");
    return;
  }

  relaySet(true);
  pumpRunning = true;
  lastCmd = reason;

  Serial.println("Pump ON: " + reason);
}

void pumpOFF(String reason) {
  relaySet(false);
  pumpRunning = false;
  lastCmd = reason;

  Serial.println("Pump OFF: " + reason);
}

// ================= Soil Sensor Fetch =================
bool fetchSensorReading() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot fetch soil sensor.");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(SENSOR_IP) + "/read";

  Serial.println("Requesting soil sensor from: " + url);

  http.begin(url);
  http.setTimeout(1500);

  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    Serial.println("Sensor Response: " + body);

    int newRaw = 0;
    int newPercent = 0;
    bool newIsDry = false;

    bool okRaw = extractIntValue(body, "raw", newRaw);
    bool okPercent = extractIntValue(body, "percent", newPercent);
    bool okDry = extractBoolValue(body, "isDry", newIsDry);

    http.end();

    if (!okRaw || !okPercent || !okDry) {
      Serial.println("JSON parse error. Make sure sensor returns: {\"raw\":123,\"percent\":45,\"isDry\":true}");
      return false;
    }

    soilRaw = newRaw;
    soilPercent = newPercent;
    isDry = newIsDry;
    readingDone = true;

    return true;
  }

  Serial.print("Failed to fetch soil reading. HTTP code = ");
  Serial.println(code);

  http.end();
  return false;
}

// ================= Auto Control =================
void runAutoControl() {
  if (!autoMode) return;
  if (overrideMode) return;
  if (!readingDone) return;

  syncPumpState();

  if (isDry && !pumpRunning) {
    pumpON("Auto Mode - Soil Dry");
  } else if (!isDry && pumpRunning) {
    pumpOFF("Auto Mode - Soil Wet");
  }
}

// ================= Web Page =================
String buildPage() {
  syncPumpState();

  String page = String(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Farm</title>

  <style>
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }

    body {
      font-family: Arial, sans-serif;
      background: #f0f4f8;
      padding: 20px;
    }

    .card {
      background: white;
      border-radius: 16px;
      padding: 24px;
      max-width: 520px;
      margin: 0 auto 20px;
      box-shadow: 0 4px 16px rgba(0,0,0,0.1);
    }

    h1 {
      text-align: center;
      color: #2c3e50;
      margin-bottom: 20px;
      font-size: 22px;
    }

    .row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 0;
      border-bottom: 1px solid #eee;
      gap: 12px;
    }

    .label-ar {
      color: #555;
      font-size: 15px;
    }

    .label-en {
      color: #aaa;
      font-size: 12px;
    }

    .value {
      font-weight: bold;
      font-size: 16px;
      text-align: right;
    }

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

    .btn-auto    { background: #3498db; color: white; }
    .btn-manual  { background: #e67e22; color: white; }
    .btn-on      { background: #27ae60; color: white; }
    .btn-off     { background: #e74c3c; color: white; }
    .btn-refresh { background: #8e44ad; color: white; }

    .btn:hover {
      opacity: 0.9;
    }

    .divider {
      border: none;
      border-top: 2px dashed #ddd;
      margin: 18px 0;
    }

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

    .timer-input {
      width: 100%;
      padding: 12px;
      margin: 8px 0;
      border-radius: 10px;
      border: 1px solid #ccc;
      font-size: 16px;
    }

    small {
      font-size: 12px;
    }
  </style>

  <script>
    function setText(id, value) {
      const el = document.getElementById(id);
      if (el) el.textContent = value;
    }

    function setHTML(id, value) {
      const el = document.getElementById(id);
      if (el) el.innerHTML = value;
    }

    function setColor(id, color) {
      const el = document.getElementById(id);
      if (el) el.style.color = color;
    }

    function setBackground(id, color) {
      const el = document.getElementById(id);
      if (el) el.style.background = color;
    }

    function updateWater(data) {
      setText("waterRaw", data.waterRaw);
      setText("waterPercent", data.waterPercent + "%");

      let statusHTML = "";
      let alertHTML = "";
      let color = "#27ae60";

      if (data.waterPercent >= 70) {
        statusHTML = "ممتاز 🟢<br><small>Good</small>";
        alertHTML = "✅ مستوى الماء ممتاز<br><small>Water level is good</small>";
        color = "#27ae60";
      } else if (data.waterPercent >= data.minWaterPercent) {
        statusHTML = "متوسط 🟠<br><small>Medium</small>";
        alertHTML = "⚠️ مستوى الماء متوسط<br><small>Water level is medium</small>";
        color = "#f39c12";
      } else {
        statusHTML = "فارغ / منخفض ⛔<br><small>Empty / Low</small>";
        alertHTML = "⛔ الماء منخفض - المضخة محجوبة<br><small>Water level is low - pump blocked</small>";
        color = "#e74c3c";
      }

      setHTML("waterVal", statusHTML);
      setColor("waterVal", color);
      setHTML("waterAlert", alertHTML);
      setBackground("waterAlert", color);
    }

    function updateSoil(data) {
      if (!data.readingDone) {
        setText("soilRaw", "--");
        setText("soilPercent", "--");
        setHTML("soilVal", "لم تتم القراءة بعد<br><small>No reading yet</small>");
        setColor("soilVal", "#888");
        setHTML("soilAlert", "جاري انتظار القراءة التلقائية<br><small>Waiting for automatic reading</small>");
        setBackground("soilAlert", "#95a5a6");
        return;
      }

      setText("soilRaw", data.soilRaw);
      setText("soilPercent", data.soilPercent + "%");

      if (data.isDry) {
        setHTML("soilVal", "جافة 🔴<br><small>Dry</small>");
        setColor("soilVal", "#e74c3c");
        setHTML("soilAlert", "⚠️ التربة تحتاج ري<br><small>Soil needs irrigation</small>");
        setBackground("soilAlert", "#e74c3c");
      } else {
        setHTML("soilVal", "مبللة 🟢<br><small>Wet</small>");
        setColor("soilVal", "#27ae60");
        setHTML("soilAlert", "✅ التربة ممتازة<br><small>Soil is fine</small>");
        setBackground("soilAlert", "#27ae60");
      }
    }

    function updatePump(data) {
      if (data.pumpRunning) {
        setHTML("pumpVal", "شغالة 🟢<br><small>Running</small>");
        setColor("pumpVal", "#27ae60");
      } else {
        setHTML("pumpVal", "متوقفة 🔴<br><small>Stopped</small>");
        setColor("pumpVal", "#e74c3c");
      }

      if (data.autoMode) {
        setHTML("modeVal", "تلقائي 🤖<br><small>Automatic</small>");
        setColor("modeVal", "#3498db");
      } else {
        setHTML("modeVal", "يدوي ✋<br><small>Manual</small>");
        setColor("modeVal", "#e67e22");
      }

      if (data.overrideMode) {
        setHTML("overrideVal", "مفعل 🟣<br><small>Enabled</small>");
        setColor("overrideVal", "#8e44ad");
      } else {
        setHTML("overrideVal", "متوقف ⚫<br><small>Disabled</small>");
        setColor("overrideVal", "#7f8c8d");
      }

      if (data.powerEnabled) {
        setHTML("powerVal", "متصلة ⚡<br><small>Connected</small>");
        setColor("powerVal", "#27ae60");
      } else {
        setHTML("powerVal", "مقطوعة ⛔<br><small>Disconnected</small>");
        setColor("powerVal", "#e74c3c");
      }

      setText("timerVal", data.overrideSeconds + " sec");
      setText("lastCmd", data.lastCmd);
    }

    function updateDashboard() {
      fetch("/status")
        .then(r => r.json())
        .then(data => {
          updateWater(data);
          updateSoil(data);
          updatePump(data);
        })
        .catch(err => console.log("Status update failed", err));
    }

    window.onload = function() {
      updateDashboard();
      setInterval(updateDashboard, 2000);
    };
  </script>
</head>

<body>
  <div class="card">
    <h1>💧 Water Tank / خزان الماء</h1>

    <div class="row">
      <div>
        <div class="label-ar">القراءة الخام</div>
        <div class="label-en">Raw Reading</div>
      </div>
      <div class="value" id="waterRaw">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">نسبة الماء</div>
        <div class="label-en">Water Percentage</div>
      </div>
      <div class="value" id="waterPercent">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">حالة الماء</div>
        <div class="label-en">Water Status</div>
      </div>
      <div class="value" id="waterVal">--</div>
    </div>

    <div class="alert-box" id="waterAlert" style="background:#95a5a6">
      جاري التحديث تلقائيا<br>
      <small>Updating automatically</small>
    </div>
  </div>

  <div class="card">
    <h1>🌱 Smart Farm Dashboard</h1>

    <div class="section-title">Soil Sensor</div>

    <div class="row">
      <div>
        <div class="label-ar">القيمة الخام</div>
        <div class="label-en">Raw Value</div>
      </div>
      <div class="value" id="soilRaw">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">نسبة الرطوبة</div>
        <div class="label-en">Moisture Percentage</div>
      </div>
      <div class="value" id="soilPercent">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">حالة التربة</div>
        <div class="label-en">Soil Status</div>
      </div>
      <div class="value" id="soilVal">--</div>
    </div>

    <div class="alert-box" id="soilAlert" style="background:#95a5a6">
      جاري انتظار القراءة التلقائية<br>
      <small>Waiting for automatic reading</small>
    </div>

    <hr class="divider">

    <div class="section-title">Pump Status</div>

    <div class="row">
      <div>
        <div class="label-ar">المضخة</div>
        <div class="label-en">Pump</div>
      </div>
      <div class="value" id="pumpVal">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">الوضع</div>
        <div class="label-en">Mode</div>
      </div>
      <div class="value" id="modeVal">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">الكهرباء</div>
        <div class="label-en">Power</div>
      </div>
      <div class="value" id="powerVal">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">آخر أمر</div>
        <div class="label-en">Last Command</div>
      </div>
      <div class="value" id="lastCmd" style="font-size:13px">--</div>
    </div>

    <hr class="divider">

    <form action="/mode-auto" method="POST">
      <button class="btn btn-auto" type="submit">🤖 Auto Mode</button>
    </form>

    <form action="/mode-manual" method="POST">
      <button class="btn btn-manual" type="submit">✋ Manual Mode</button>
    </form>

    <hr class="divider">

    <div class="section-title">Override Mode</div>

    <div class="row">
      <div>
        <div class="label-ar">حالة الأوفررايد</div>
        <div class="label-en">Override Status</div>
      </div>
      <div class="value" id="overrideVal">--</div>
    </div>

    <div class="row">
      <div>
        <div class="label-ar">المؤقت</div>
        <div class="label-en">Timer</div>
      </div>
      <div class="value" id="timerVal">--</div>
    </div>

    <form action="/set-timer" method="POST">
      <input class="timer-input" type="number" name="seconds" min="1" max="3600" value="10">
      <button class="btn btn-refresh" type="submit">⏱️ Set Timer</button>
    </form>

    <form action="/override-on" method="POST">
      <button class="btn btn-on" type="submit">🟣 Override ON</button>
    </form>

    <form action="/override-off" method="POST">
      <button class="btn btn-off" type="submit">⚫ Override OFF</button>
    </form>
)rawliteral");

  if (!autoMode) {
    page += String(R"rawliteral(
    <form action="/on" method="POST">
      <button class="btn btn-on" type="submit">🟢 Pump ON</button>
    </form>

    <form action="/off" method="POST">
      <button class="btn btn-off" type="submit">🔴 Pump OFF</button>
    </form>
)rawliteral");
  } else {
    page += String(R"rawliteral(
    <div class="info-box">
      Auto mode is enabled
    </div>
)rawliteral");
  }

  page += String(R"rawliteral(
  </div>
</body>
</html>
)rawliteral");

  return page;
}

// ================= Routes =================
void handleHome() {
  server.send(200, "text/html", buildPage());
}

void handleStatus() {
  syncPumpState();

  String json = "{";
  json += "\"waterRaw\":" + String(waterRaw) + ",";
  json += "\"waterPercent\":" + String(waterPercent) + ",";
  json += "\"waterOK\":" + String(waterOK ? "true" : "false") + ",";
  json += "\"minWaterPercent\":" + String(MIN_WATER_PERCENT_TO_RUN) + ",";

  json += "\"soilRaw\":" + String(soilRaw) + ",";
  json += "\"soilPercent\":" + String(soilPercent) + ",";
  json += "\"isDry\":" + String(isDry ? "true" : "false") + ",";
  json += "\"readingDone\":" + String(readingDone ? "true" : "false") + ",";

  json += "\"pumpRunning\":" + String(pumpRunning ? "true" : "false") + ",";
  json += "\"autoMode\":" + String(autoMode ? "true" : "false") + ",";
  json += "\"overrideMode\":" + String(overrideMode ? "true" : "false") + ",";
  json += "\"powerEnabled\":" + String(powerEnabled ? "true" : "false") + ",";
  json += "\"overrideSeconds\":" + String(overrideSeconds) + ",";
  json += "\"lastCmd\":\"" + jsonEscape(lastCmd) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleRead() {
  bool success = fetchSensorReading();

  if (success) {
    runAutoControl();
  }

  redirectHome();
}

void handlePumpOn() {
  if (!autoMode && !overrideMode) {
    pumpON("Manual ON");
  }

  redirectHome();
}

void handlePumpOff() {
  if (overrideMode) {
    overrideMode = false;
    pumpOFF("Override Manual OFF");
  } else if (!autoMode) {
    pumpOFF("Manual OFF");
  }

  redirectHome();
}

void handleModeAuto() {
  autoMode = true;
  overrideMode = false;
  lastCmd = "Auto Mode Enabled";

  fetchSensorReading();
  runAutoControl();

  redirectHome();
}

void handleModeManual() {
  autoMode = false;
  overrideMode = false;

  if (pumpRunning) {
    pumpOFF("Switched to Manual");
  } else {
    lastCmd = "Manual Mode Enabled";
  }

  redirectHome();
}

void handleOverrideOn() {
  overrideMode = true;
  overrideStart = millis();
  overrideDurationMs = (unsigned long)overrideSeconds * 1000UL;

  forcePumpON("Override ON");

  redirectHome();
}

void handleOverrideOff() {
  overrideMode = false;
  pumpOFF("Override OFF");

  redirectHome();
}

void handleSetTimer() {
  if (server.hasArg("seconds")) {
    int sec = server.arg("seconds").toInt();

    if (sec < 1) sec = 1;
    if (sec > 3600) sec = 3600;

    overrideSeconds = sec;
    overrideDurationMs = (unsigned long)overrideSeconds * 1000UL;

    lastCmd = "Override timer set to " + String(overrideSeconds) + " sec";
  }

  redirectHome();
}

// ================= Setup =================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  relaySet(false);
  pumpRunning = false;

  delay(200);

  analogReadResolution(12);
  analogSetPinAttenuation(WATER_PIN, ADC_11db);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected!");
  Serial.print("Main Controller IP: http://");
  Serial.println(WiFi.localIP());

  readWaterLevel();
  fetchSensorReading();
  runAutoControl();

  Serial.println("=== Water Calibration ===");
  Serial.print("EMPTY RAW = ");
  Serial.println(WATER_EMPTY_RAW);
  Serial.print("FULL RAW  = ");
  Serial.println(WATER_FULL_RAW);

  server.on("/", HTTP_GET, handleHome);
  server.on("/status", HTTP_GET, handleStatus);

  // Old manual read route kept as backup, but no button uses it now
  server.on("/read", HTTP_POST, handleRead);

  server.on("/on", HTTP_POST, handlePumpOn);
  server.on("/off", HTTP_POST, handlePumpOff);

  server.on("/mode-auto", HTTP_POST, handleModeAuto);
  server.on("/mode-manual", HTTP_POST, handleModeManual);

  server.on("/override-on", HTTP_POST, handleOverrideOn);
  server.on("/override-off", HTTP_POST, handleOverrideOff);
  server.on("/set-timer", HTTP_POST, handleSetTimer);

  server.begin();

  lastWaterUpdate = millis();
  lastSoilUpdate = millis();
}

// ================= Loop =================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Auto water update
  if (now - lastWaterUpdate >= WATER_UPDATE_INTERVAL) {
    lastWaterUpdate = now;
    readWaterLevel();
  }

  // Auto soil/moisture update
  if (now - lastSoilUpdate >= SOIL_UPDATE_INTERVAL) {
    lastSoilUpdate = now;

    bool success = fetchSensorReading();

    if (success) {
      runAutoControl();
    }
  }

  // Override mode control
  if (overrideMode) {
    if (!powerEnabled) {
      overrideMode = false;
      pumpOFF("Override stopped - Power Disabled");
    } else {
      if (!pumpRunning) {
        forcePumpON("Override keep running");
      }

      if (now - overrideStart >= overrideDurationMs) {
        overrideMode = false;
        pumpOFF("Override Timer Finished");
      }
    }
  }

  // Water safety protection
  if (!overrideMode && pumpRunning) {
    if (!waterOK) {
      pumpOFF("Water Too Low");
      Serial.println("Pump stopped بسبب انخفاض الماء");
    }
  }

  // Debug
  if (now - lastDebug >= DEBUG_INTERVAL) {
    lastDebug = now;

    syncPumpState();

    Serial.print("Relay pin state = ");
    Serial.print(digitalRead(RELAY_PIN));

    Serial.print(" | pumpRunning = ");
    Serial.print(pumpRunning ? "ON" : "OFF");

    Serial.print(" | autoMode = ");
    Serial.print(autoMode ? "ON" : "OFF");

    Serial.print(" | overrideMode = ");
    Serial.print(overrideMode ? "ON" : "OFF");

    Serial.print(" | water = ");
    Serial.print(waterPercent);
    Serial.print("%");

    Serial.print(" | soil = ");
    Serial.print(readingDone ? String(soilPercent) + "%" : "No Reading");

    Serial.print(" | isDry = ");
    Serial.println(isDry ? "true" : "false");
  }
}

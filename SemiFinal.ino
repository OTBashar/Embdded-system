#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ================= WiFi =================
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "PUT_YOUR_WIFI_PASSWORD_HERE";

// ===== IP of System 1 Soil Sensor ESP32 =====
const char* SENSOR_IP = "172.20.10.12";

// ================= Pins =================
#define RELAY_PIN 26
#define WATER_PIN 34
#define RELAY_ACTIVE_LOW true

// ================= Settings =================
#define WATER_SAMPLES 31
#define ADC_SETTLE_DELAY 3

const unsigned long WATER_UPDATE_INTERVAL_MS = 2000;
const unsigned long SOIL_UPDATE_INTERVAL_MS  = 5000;
const unsigned long DEBUG_INTERVAL_MS        = 2000;

// ================= Water Calibration =================
// Change these after calibration if needed.
const int WATER_EMPTY_RAW = 850;
const int WATER_FULL_RAW  = 1500;

// ================= Thresholds =================
const int PERCENT_HYSTERESIS = 3;
const int MIN_WATER_PERCENT_TO_RUN = 20;

// ================= Server =================
WebServer server(80);

// ================= System State =================
bool pumpRunning  = false;
bool autoMode     = true;
bool readingDone  = false;
bool powerEnabled = true;   // Kept internally, but no cut-power button in UI.

bool overrideMode = false;
int overrideSeconds = 10;
unsigned long overrideStart = 0;
unsigned long overrideDurationMs = 10000;

int soilRaw = 0;
int soilPercent = 0;
bool isDry = false;
bool soilOnline = false;

int waterRaw = 0;
int waterPercent = 0;
int lastWaterPercent = 0;
bool waterOK = true;

String lastCmd = "None / لا يوجد";

unsigned long lastWaterUpdate = 0;
unsigned long lastSoilUpdate = 0;
unsigned long lastDebug = 0;

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

  waterOK = waterPercent >= MIN_WATER_PERCENT_TO_RUN;

  Serial.print("Water Raw: ");
  Serial.print(waterRaw);
  Serial.print(" | Water Percent: ");
  Serial.print(waterPercent);
  Serial.println("%");
}

// ================= Simple JSON Parsing =================
bool getJsonInt(const String& body, const char* key, int& value) {
  String token = "\"" + String(key) + "\"";
  int keyIndex = body.indexOf(token);
  if (keyIndex < 0) return false;

  int colon = body.indexOf(':', keyIndex);
  if (colon < 0) return false;

  int start = colon + 1;
  while (start < body.length() && (body[start] == ' ' || body[start] == '\t')) {
    start++;
  }

  int end = start;
  while (end < body.length() &&
         ((body[end] >= '0' && body[end] <= '9') || body[end] == '-')) {
    end++;
  }

  if (end == start) return false;

  value = body.substring(start, end).toInt();
  return true;
}

bool getJsonBool(const String& body, const char* key, bool& value) {
  String token = "\"" + String(key) + "\"";
  int keyIndex = body.indexOf(token);
  if (keyIndex < 0) return false;

  int colon = body.indexOf(':', keyIndex);
  if (colon < 0) return false;

  int start = colon + 1;
  while (start < body.length() && (body[start] == ' ' || body[start] == '\t')) {
    start++;
  }

  String rest = body.substring(start);
  rest.trim();

  if (rest.startsWith("true")) {
    value = true;
    return true;
  }

  if (rest.startsWith("false")) {
    value = false;
    return true;
  }

  return false;
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "");
  return s;
}

// ================= Pump Control =================
void pumpON(String reason) {
  readWaterLevel();

  if (!powerEnabled) {
    relaySet(false);
    pumpRunning = false;
    lastCmd = "Power Disabled";
    Serial.println("Pump blocked: power disabled");
    return;
  }

  if (!waterOK) {
    relaySet(false);
    pumpRunning = false;
    lastCmd = "Water Low - Pump Blocked";
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

void servicePumpSafety() {
  syncPumpState();

  if (pumpRunning && !waterOK) {
    overrideMode = false;
    pumpOFF("Water Too Low");
  }

  if (pumpRunning && !powerEnabled) {
    overrideMode = false;
    pumpOFF("Power Disabled");
  }
}

// ================= Soil Sensor Fetch =================
bool fetchSensorReading() {
  if (WiFi.status() != WL_CONNECTED) {
    soilOnline = false;
    Serial.println("WiFi not connected. Soil fetch skipped.");
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

    bool okRaw = getJsonInt(body, "raw", newRaw);
    bool okPercent = getJsonInt(body, "percent", newPercent);
    bool okDry = getJsonBool(body, "isDry", newIsDry);

    http.end();

    if (!okRaw || !okPercent || !okDry) {
      soilOnline = false;
      Serial.println("Soil JSON parse error");
      return false;
    }

    soilRaw = newRaw;
    soilPercent = newPercent;
    isDry = newIsDry;
    readingDone = true;
    soilOnline = true;

    return true;
  }

  Serial.print("Failed to fetch soil reading. HTTP code = ");
  Serial.println(code);

  soilOnline = false;
  http.end();

  return false;
}

// ================= Auto Mode Logic =================
void performAutoControl() {
  if (!autoMode || overrideMode || !readingDone) return;

  syncPumpState();

  if (!powerEnabled) {
    if (pumpRunning) pumpOFF("Auto stopped - Power Disabled");
    return;
  }

  if (!waterOK) {
    if (pumpRunning) {
      pumpOFF("Auto stopped - Water Low");
    } else {
      lastCmd = "Auto blocked - Water Low";
    }
    return;
  }

  if (isDry && !pumpRunning) {
    pumpON("Auto ON - Soil Dry");
  } else if (!isDry && pumpRunning) {
    pumpOFF("Auto OFF - Soil Wet");
  }
}

void serviceOverrideTimer() {
  if (!overrideMode) return;

  syncPumpState();

  if (!waterOK) {
    overrideMode = false;
    pumpOFF("Override stopped - Water Low");
    return;
  }

  if (!powerEnabled) {
    overrideMode = false;
    pumpOFF("Override stopped - Power Disabled");
    return;
  }

  if (!pumpRunning) {
    pumpON("Override keep running");
  }

  if (millis() - overrideStart >= overrideDurationMs) {
    overrideMode = false;
    pumpOFF("Override Timer Finished");
  }
}

// ================= Web Page =================
String buildPage() {
  return R"rawliteral(
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
      color: #2c3e50;
    }

    .card {
      background: white;
      border-radius: 16px;
      padding: 24px;
      max-width: 560px;
      margin: 0 auto 20px;
      box-shadow: 0 4px 16px rgba(0,0,0,0.1);
    }

    h1 {
      text-align: center;
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
      color: #999;
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
      color: white;
    }

    .btn-auto { background: #3498db; }
    .btn-manual { background: #e67e22; }
    .btn-on { background: #27ae60; }
    .btn-off { background: #e74c3c; }
    .btn-timer { background: #8e44ad; }

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

    .live {
      text-align: center;
      font-size: 13px;
      color: #777;
      margin-bottom: 12px;
    }

    small {
      font-weight: normal;
    }
  </style>
</head>

<body>
  <div class="card">
    <h1>💧 Water Tank / خزان الماء</h1>
    <div class="live" id="conn">Connecting...</div>

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

    <div class="alert-box" id="waterAlert">Loading...</div>
  </div>

  <div class="card">
    <h1>🌱 Smart Farm Dashboard</h1>

    <div class="section-title">Soil Sensor</div>

    <div class="row">
      <div>
        <div class="label-ar">حالة الاتصال</div>
        <div class="label-en">Sensor Connection</div>
      </div>
      <div class="value" id="soilOnline">--</div>
    </div>

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

    <div class="alert-box" id="soilAlert">Loading...</div>

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

    <div id="autoInfo" class="info-box">
      Auto mode is enabled. Soil and water update by themselves.
    </div>

    <hr class="divider">

    <div class="section-title">Override Mode</div>

    <div class="row">
      <div>
        <div class="label-ar">حالة الأوفررايد</div>
        <div class="label-en">Override Status</div>
      </div>
      <div class="value" id="overrideVal">--</div>
    </div>

    <form action="/set-timer" method="POST">
      <input id="timerInput" class="timer-input" type="number" name="seconds" min="1" max="3600" value="10">
      <button class="btn btn-timer" type="submit">⏱️ Set Timer</button>
    </form>

    <form action="/override-on" method="POST">
      <button class="btn btn-on" type="submit">🟣 Override ON</button>
    </form>

    <form action="/override-off" method="POST">
      <button class="btn btn-off" type="submit">⚫ Override OFF</button>
    </form>

    <div id="manualControls">
      <hr class="divider">

      <form action="/on" method="POST">
        <button class="btn btn-on" type="submit">🟢 Pump ON</button>
      </form>

      <form action="/off" method="POST">
        <button class="btn btn-off" type="submit">🔴 Pump OFF</button>
      </form>
    </div>
  </div>

  <script>
    const POLL_MS = 2000;

    function setWaterStatus(d) {
      const raw = d.waterRaw;
      const percent = d.waterPercent;

      document.getElementById("waterRaw").innerText = raw;
      document.getElementById("waterPercent").innerText = percent + "%";

      const waterVal = document.getElementById("waterVal");
      const waterAlert = document.getElementById("waterAlert");

      if (percent >= 70) {
        waterVal.style.color = "#27ae60";
        waterVal.innerHTML = "ممتاز 🟢<br><small>Good</small>";
        waterAlert.style.background = "#27ae60";
        waterAlert.innerHTML = "✅ مستوى الماء ممتاز<br><small>Water level is good</small>";
      } else if (percent >= d.minPercent) {
        waterVal.style.color = "#f39c12";
        waterVal.innerHTML = "متوسط 🟠<br><small>Medium</small>";
        waterAlert.style.background = "#f39c12";
        waterAlert.innerHTML = "⚠️ مستوى الماء متوسط<br><small>Water level is medium</small>";
      } else {
        waterVal.style.color = "#e74c3c";
        waterVal.innerHTML = "فارغ / منخفض ⛔<br><small>Empty / Low</small>";
        waterAlert.style.background = "#e74c3c";
        waterAlert.innerHTML = "⛔ الماء منخفض - المضخة محجوبة<br><small>Water level is low - pump blocked</small>";
      }
    }

    function setSoilStatus(d) {
      const soilOnline = document.getElementById("soilOnline");
      const soilVal = document.getElementById("soilVal");
      const soilAlert = document.getElementById("soilAlert");

      if (d.soilOnline) {
        soilOnline.style.color = "#27ae60";
        soilOnline.innerHTML = "متصل 🟢<br><small>Online</small>";
      } else {
        soilOnline.style.color = "#e74c3c";
        soilOnline.innerHTML = "غير متصل 🔴<br><small>Offline</small>";
      }

      if (!d.readingDone) {
        document.getElementById("soilRaw").innerText = "--";
        document.getElementById("soilPercent").innerText = "--";

        soilVal.style.color = "#7f8c8d";
        soilVal.innerHTML = "لم تتم القراءة بعد<br><small>No reading yet</small>";

        soilAlert.style.background = "#95a5a6";
        soilAlert.innerHTML = "انتظار أول قراءة<br><small>Waiting for first reading</small>";
        return;
      }

      document.getElementById("soilRaw").innerText = d.soilRaw;
      document.getElementById("soilPercent").innerText = d.soilPercent + "%";

      if (d.isDry) {
        soilVal.style.color = "#e74c3c";
        soilVal.innerHTML = "جافة 🔴<br><small>Dry</small>";

        soilAlert.style.background = "#e74c3c";
        soilAlert.innerHTML = "⚠️ التربة تحتاج ري<br><small>Soil needs irrigation</small>";
      } else {
        soilVal.style.color = "#27ae60";
        soilVal.innerHTML = "مبللة 🟢<br><small>Wet</small>";

        soilAlert.style.background = "#27ae60";
        soilAlert.innerHTML = "✅ التربة ممتازة<br><small>Soil is fine</small>";
      }

      if (!d.soilOnline) {
        soilAlert.style.background = "#e67e22";
        soilAlert.innerHTML = "⚠️ الحساس غير متصل - يتم عرض آخر قراءة<br><small>Sensor offline - showing last saved reading</small>";
      }
    }

    function setPumpStatus(d) {
      const pumpVal = document.getElementById("pumpVal");
      const modeVal = document.getElementById("modeVal");
      const overrideVal = document.getElementById("overrideVal");

      if (d.pumpRunning) {
        pumpVal.style.color = "#27ae60";
        pumpVal.innerHTML = "شغالة 🟢<br><small>Running</small>";
      } else {
        pumpVal.style.color = "#e74c3c";
        pumpVal.innerHTML = "متوقفة 🔴<br><small>Stopped</small>";
      }

      if (d.autoMode) {
        modeVal.style.color = "#3498db";
        modeVal.innerHTML = "تلقائي 🤖<br><small>Automatic</small>";
      } else {
        modeVal.style.color = "#e67e22";
        modeVal.innerHTML = "يدوي ✋<br><small>Manual</small>";
      }

      if (d.overrideMode) {
        overrideVal.style.color = "#8e44ad";
        overrideVal.innerHTML = "مفعل 🟣<br><small>Enabled</small>";
      } else {
        overrideVal.style.color = "#7f8c8d";
        overrideVal.innerHTML = "متوقف ⚫<br><small>Disabled</small>";
      }

      document.getElementById("lastCmd").innerText = d.lastCmd;

      const timerInput = document.getElementById("timerInput");
      if (document.activeElement !== timerInput) {
        timerInput.value = d.overrideSeconds;
      }

      document.getElementById("manualControls").style.display =
        (!d.autoMode && !d.overrideMode) ? "block" : "none";

      document.getElementById("autoInfo").style.display =
        d.autoMode ? "block" : "none";
    }

    async function loadStatus() {
      try {
        const response = await fetch("/status?ts=" + Date.now());
        const d = await response.json();

        document.getElementById("conn").innerText =
          "Live update: water every 2s, soil every 5s";

        setWaterStatus(d);
        setSoilStatus(d);
        setPumpStatus(d);
      } catch (e) {
        document.getElementById("conn").innerText = "Connection lost...";
      }
    }

    window.addEventListener("load", () => {
      loadStatus();
      setInterval(loadStatus, POLL_MS);
    });
  </script>
</body>
</html>
)rawliteral";
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
  json += "\"minPercent\":" + String(MIN_WATER_PERCENT_TO_RUN) + ",";

  json += "\"soilRaw\":" + String(soilRaw) + ",";
  json += "\"soilPercent\":" + String(soilPercent) + ",";
  json += "\"isDry\":" + String(isDry ? "true" : "false") + ",";
  json += "\"readingDone\":" + String(readingDone ? "true" : "false") + ",";
  json += "\"soilOnline\":" + String(soilOnline ? "true" : "false") + ",";

  json += "\"pumpRunning\":" + String(pumpRunning ? "true" : "false") + ",";
  json += "\"autoMode\":" + String(autoMode ? "true" : "false") + ",";
  json += "\"overrideMode\":" + String(overrideMode ? "true" : "false") + ",";
  json += "\"overrideSeconds\":" + String(overrideSeconds) + ",";
  json += "\"powerEnabled\":" + String(powerEnabled ? "true" : "false") + ",";
  json += "\"lastCmd\":\"" + jsonEscape(lastCmd) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleModeAuto() {
  autoMode = true;
  overrideMode = false;

  if (readingDone) {
    performAutoControl();
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeManual() {
  autoMode = false;
  overrideMode = false;

  if (pumpRunning) {
    pumpOFF("Switched to Manual");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOn() {
  if (!autoMode && !overrideMode) {
    pumpON("Manual ON");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOff() {
  if (overrideMode) {
    overrideMode = false;
    pumpOFF("Override Manual OFF");
  } else if (!autoMode) {
    pumpOFF("Manual OFF");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOverrideOn() {
  overrideMode = true;
  overrideStart = millis();
  overrideDurationMs = (unsigned long)overrideSeconds * 1000UL;

  pumpON("Override ON");

  if (!pumpRunning) {
    overrideMode = false;
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOverrideOff() {
  overrideMode = false;
  pumpOFF("Override OFF");

  server.sendHeader("Location", "/");
  server.send(303);
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

  server.sendHeader("Location", "/");
  server.send(303);
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

  Serial.println("\nConnected!");
  Serial.print("Main Controller IP: http://");
  Serial.println(WiFi.localIP());

  readWaterLevel();
  fetchSensorReading();

  Serial.println("=== Water Calibration ===");
  Serial.print("EMPTY RAW = ");
  Serial.println(WATER_EMPTY_RAW);
  Serial.print("FULL RAW  = ");
  Serial.println(WATER_FULL_RAW);

  server.on("/", HTTP_GET, handleHome);
  server.on("/status", HTTP_GET, handleStatus);

  server.on("/mode-auto", HTTP_POST, handleModeAuto);
  server.on("/mode-manual", HTTP_POST, handleModeManual);

  server.on("/on", HTTP_POST, handlePumpOn);
  server.on("/off", HTTP_POST, handlePumpOff);

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

  if (now - lastWaterUpdate >= WATER_UPDATE_INTERVAL_MS) {
    lastWaterUpdate = now;

    readWaterLevel();
    servicePumpSafety();
  }

  if (now - lastSoilUpdate >= SOIL_UPDATE_INTERVAL_MS) {
    lastSoilUpdate = now;

    bool success = fetchSensorReading();

    if (success) {
      performAutoControl();
    }
  }

  serviceOverrideTimer();

  if (now - lastDebug >= DEBUG_INTERVAL_MS) {
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
    Serial.print(readingDone ? String(soilPercent) + "%" : "NO READING");

    Serial.print(" | isDry = ");
    Serial.println(isDry ? "true" : "false");
  }
}
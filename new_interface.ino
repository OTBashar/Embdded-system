#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ─── Network Settings / إعدادات الشبكة ─────────────────────────
const char* WIFI_SSID = "iPhoneAir";
const char* WIFI_PASS = "99998888";

// ⚠️ ضع IP الحساس هنا / Put Sensor IP here
const char* SENSOR_IP = "192.168.1.105";

// ─── Pins / المنافذ ─────────────────────────────────────────────
#define RELAY_PIN  26
#define WATER_PIN  34   // Analog water sensor on ADC1

// ─── Settings / الإعدادات ───────────────────────────────────────
#define PUMP_DURATION 10000
#define WATER_SAMPLES 20

// EMPTY = reading when tank is empty
// FULL  = reading when tank is full
int WATER_EMPTY_RAW = 1200;
int WATER_FULL_RAW  = 3000;

// minimum water percent required to allow pump run
int MIN_WATER_PERCENT_TO_RUN = 15;

// ─── Variables / المتغيرات ─────────────────────────────────────
WebServer server(80);

bool pumpRunning   = false;
bool autoMode      = true;
bool readingDone   = false;
bool powerEnabled  = true;

int  soilRaw       = 0;
int  soilPercent   = 0;
bool isDry         = false;

int  waterRaw      = 0;
int  waterPercent  = 0;
bool waterOK       = true;

String lastCmd     = "None / لا يوجد";
unsigned long pumpStart = 0;

// ─── Water Sensor Helpers / دوال حساس الماء ────────────────────
int readWaterAverage(int pin, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(5);
  }
  return sum / samples;
}

int calculateWaterPercent(int raw) {
  int percent = map(raw, WATER_EMPTY_RAW, WATER_FULL_RAW, 0, 100);

  // If percentage is reversed, replace the line above with:
  // int percent = map(raw, WATER_FULL_RAW, WATER_EMPTY_RAW, 0, 100);

  percent = constrain(percent, 0, 100);
  return percent;
}

void readWaterLevel() {
  waterRaw = readWaterAverage(WATER_PIN, WATER_SAMPLES);
  waterPercent = calculateWaterPercent(waterRaw);
  waterOK = (waterPercent >= MIN_WATER_PERCENT_TO_RUN);
}

// ─── Pump Control / التحكم بالمضخة ─────────────────────────────
void pumpON(String reason) {
  readWaterLevel();

  if (!powerEnabled) {
    lastCmd = "⛔ الكهرباء مقطوعة / Power Disabled";
    Serial.println("⚠️ Pump BLOCKED — Power is OFF!");
    return;
  }

  if (!waterOK) {
    lastCmd = "⛔ حُجبت — الماء منخفض / Blocked — Low Water";
    Serial.println("⚠️ Pump BLOCKED — Water level too low!");
    return;
  }

  digitalWrite(RELAY_PIN, HIGH);
  pumpRunning = true;
  pumpStart   = millis();
  lastCmd     = reason;

  Serial.println("✅ Pump ON — " + reason);
}

void pumpOFF(String reason) {
  digitalWrite(RELAY_PIN, LOW);
  pumpRunning = false;
  lastCmd     = reason;

  Serial.println("🔴 Pump OFF — " + reason);
}

// ─── Get Reading from Sensor ESP32 / جلب قراءة حساس التربة ─────
bool fetchSensorReading() {
  HTTPClient http;
  String url = "http://" + String(SENSOR_IP) + "/read";

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    Serial.println("Sensor Response: " + body);

    soilRaw = body.substring(
      body.indexOf("\"raw\":") + 6,
      body.indexOf(",\"percent\"")
    ).toInt();

    soilPercent = body.substring(
      body.indexOf("\"percent\":") + 10,
      body.indexOf(",\"isDry\"")
    ).toInt();

    isDry = body.indexOf("\"isDry\":true") >= 0;
    readingDone = true;

    http.end();
    return true;
  } else {
    Serial.println("❌ Failed to get sensor reading / فشل الاتصال بحساس التربة");
    http.end();
    return false;
  }
}

// ─── Build Web Page / بناء صفحة الويب ──────────────────────────
String buildPage() {
  readWaterLevel();

  String soilState = "idle";
  String soilAr = "لم تتم القراءة بعد";
  String soilEn = "No reading yet";
  String soilAlertAr = "اضغط قراءة الحساس للبدء";
  String soilAlertEn = "Press Read Sensor to start";

  if (readingDone) {
    if (isDry) {
      soilState = "danger";
      soilAr = "جافة";
      soilEn = "Dry";
      soilAlertAr = "التربة تحتاج ري الآن";
      soilAlertEn = "Soil needs irrigation now";
    } else {
      soilState = "success";
      soilAr = "مبللة";
      soilEn = "Wet";
      soilAlertAr = "التربة لا تحتاج ري";
      soilAlertEn = "Soil does not need irrigation";
    }
  }

  String pumpState = pumpRunning ? "success" : "danger";
  String pumpAr = pumpRunning ? "شغالة" : "متوقفة";
  String pumpEn = pumpRunning ? "Running" : "Stopped";

  String modeState = autoMode ? "info" : "warning";
  String modeAr = autoMode ? "تلقائي" : "يدوي";
  String modeEn = autoMode ? "Automatic" : "Manual";

  String waterState;
  String waterAr;
  String waterEn;
  String waterAlertAr;
  String waterAlertEn;

  if (waterPercent >= 70) {
    waterState = "success";
    waterAr = "ممتاز";
    waterEn = "Excellent";
    waterAlertAr = "مستوى الماء ممتاز — جاهز للري";
    waterAlertEn = "Water level is excellent — Ready to irrigate";
  } else if (waterPercent >= MIN_WATER_PERCENT_TO_RUN) {
    waterState = "warning";
    waterAr = "متوسط";
    waterEn = "Moderate";
    waterAlertAr = "مستوى الماء متوسط";
    waterAlertEn = "Water level is moderate";
  } else {
    waterState = "danger";
    waterAr = "منخفض";
    waterEn = "Low";
    waterAlertAr = "الماء منخفض — المضخة محجوبة";
    waterAlertEn = "Water is low — Pump is blocked";
  }

  String powerState = powerEnabled ? "success" : "danger";
  String powerAr = powerEnabled ? "متصلة" : "مقطوعة";
  String powerEn = powerEnabled ? "Connected" : "Disconnected";
  String powerBtnClass = powerEnabled ? "btn-danger" : "btn-success";
  String powerBtnAr = powerEnabled ? "قطع الكهرباء" : "وصل الكهرباء";
  String powerBtnEn = powerEnabled ? "Cut Power" : "Connect Power";

  String manualSection = "";
  if (!autoMode) {
    manualSection += R"rawliteral(
      <div class='actions two-col'>
        <form action='/on' method='POST'>
          <button class='btn btn-success' type='submit' data-ar='تشغيل المضخة' data-en='Turn Pump ON'></button>
        </form>
        <form action='/off' method='POST'>
          <button class='btn btn-danger' type='submit' data-ar='إيقاف المضخة' data-en='Turn Pump OFF'></button>
        </form>
      </div>
    )rawliteral";
  } else {
    manualSection += R"rawliteral(
      <div class='note-card info'>
        <div class='note-title' data-ar='وضع تلقائي مفعل' data-en='Auto mode is active'></div>
        <div class='note-body' data-ar='المضخة تتحكم بها قراءة حساس التربة مع فحص مستوى الماء والطاقة.' data-en='The pump is controlled by soil readings with water level and power checks.'></div>
      </div>
    )rawliteral";
  }

  String page = R"rawliteral(
<!DOCTYPE html>
<html lang='en' dir='ltr'>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Smart Farm Dashboard</title>
  <style>
    :root {
      --bg: #f3efe8;
      --panel: #111216;
      --panel2: #171923;
      --panel3: #1d202b;
      --line: rgba(255,255,255,.08);
      --text: #f6f7fb;
      --muted: #aeb4c7;
      --coral: #f4a18f;
      --yellow: #e8ea82;
      --lav: #a8a3ff;
      --blue: #98b2ff;
      --green: #6edca8;
      --orange: #ffbe6b;
      --red: #ff7b7b;
      --slate: #8d95ab;
      --shadow: 0 25px 80px rgba(0,0,0,.28);
    }

    * { box-sizing: border-box; }
    html { -webkit-text-size-adjust: 100%; }
    body {
      margin: 0;
      font-family: Arial, Helvetica, sans-serif;
      background: radial-gradient(circle at top, #faf7f2 0%, var(--bg) 48%, #ebe5dc 100%);
      color: var(--text);
      padding: 18px;
    }

    .app {
      max-width: 1180px;
      margin: 0 auto;
      background: var(--panel);
      border-radius: 38px;
      overflow: hidden;
      box-shadow: var(--shadow);
    }

    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 16px;
      padding: 20px 20px 10px;
      flex-wrap: wrap;
    }

    .eyebrow {
      font-size: 11px;
      letter-spacing: .24em;
      text-transform: uppercase;
      color: var(--muted);
      margin-bottom: 8px;
    }

    .title {
      font-size: 30px;
      font-weight: 800;
      line-height: 1.05;
      margin: 0;
    }

    .subtitle {
      margin-top: 8px;
      color: var(--muted);
      font-size: 14px;
    }

    .lang-switch {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      background: var(--panel3);
      border: 1px solid var(--line);
      border-radius: 999px;
      padding: 6px;
    }

    .lang-btn {
      border: 0;
      background: transparent;
      color: var(--muted);
      border-radius: 999px;
      padding: 10px 14px;
      min-width: 62px;
      font-weight: 700;
      cursor: pointer;
    }

    .lang-btn.active {
      background: var(--coral);
      color: #141414;
    }

    .grid {
      display: grid;
      grid-template-columns: 1.2fr .95fr;
      gap: 16px;
      padding: 12px 20px 20px;
    }

    .stack { display: grid; gap: 16px; }

    .card {
      background: var(--panel2);
      border: 1px solid var(--line);
      border-radius: 26px;
      padding: 18px;
    }

    .hero {
      display: grid;
      grid-template-columns: 1.18fr .82fr;
      gap: 16px;
    }

    .hero-main {
      background: linear-gradient(180deg, #171923 0%, #13151d 100%);
    }

    .card-top {
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
      margin-bottom: 14px;
    }

    .card-kicker {
      font-size: 11px;
      color: var(--muted);
      letter-spacing: .22em;
      text-transform: uppercase;
      margin-bottom: 8px;
    }

    .card-title {
      font-size: 26px;
      font-weight: 800;
      margin: 0;
      line-height: 1.05;
    }

    .status-pill {
      border-radius: 999px;
      padding: 9px 12px;
      font-size: 12px;
      font-weight: 700;
      white-space: nowrap;
      background: #232734;
      color: var(--text);
      border: 1px solid var(--line);
    }

    .metrics {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 14px;
      margin-top: 16px;
    }

    .metric {
      border-radius: 22px;
      padding: 16px;
      color: #111;
      min-height: 140px;
    }

    .metric.coral { background: var(--coral); }
    .metric.yellow { background: var(--yellow); }
    .metric.lav { background: var(--lav); }
    .metric.blue { background: var(--blue); }

    .metric-label {
      font-size: 11px;
      font-weight: 700;
      letter-spacing: .18em;
      text-transform: uppercase;
      opacity: .72;
      margin-bottom: 10px;
    }

    .metric-value {
      font-size: 36px;
      font-weight: 800;
      line-height: 1;
      margin-bottom: 8px;
    }

    .metric-sub {
      font-size: 14px;
      opacity: .78;
    }

    .ring-wrap {
      display: flex;
      align-items: center;
      justify-content: center;
      padding-top: 10px;
    }

    .ring {
      width: 150px;
      height: 150px;
      border-radius: 50%;
      background: conic-gradient(var(--coral) 0 30%, var(--yellow) 30% 56%, var(--blue) 56% 78%, var(--lav) 78% 100%);
      display: grid;
      place-items: center;
      padding: 14px;
    }

    .ring-inner {
      width: 100%;
      height: 100%;
      border-radius: 50%;
      background: var(--panel);
      display: grid;
      place-items: center;
      text-align: center;
    }

    .ring-number {
      font-size: 34px;
      font-weight: 800;
      line-height: 1;
    }

    .ring-text {
      font-size: 12px;
      color: var(--muted);
      margin-top: 6px;
    }

    .list { display: grid; gap: 10px; }

    .row-item {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      background: var(--panel3);
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 13px 14px;
    }

    .row-left { min-width: 0; }

    .row-label {
      font-size: 12px;
      color: var(--muted);
      margin-bottom: 4px;
    }

    .row-main {
      font-size: 16px;
      font-weight: 700;
      word-break: break-word;
    }

    .badge {
      border-radius: 999px;
      padding: 8px 10px;
      font-size: 12px;
      font-weight: 700;
      white-space: nowrap;
      border: 1px solid rgba(0,0,0,.08);
    }

    .success { color: var(--green); }
    .warning { color: var(--orange); }
    .danger  { color: var(--red); }
    .info    { color: var(--blue); }
    .idle    { color: var(--slate); }

    .badge.success { background: rgba(110,220,168,.14); color: var(--green); border-color: rgba(110,220,168,.2); }
    .badge.warning { background: rgba(255,190,107,.14); color: var(--orange); border-color: rgba(255,190,107,.2); }
    .badge.danger  { background: rgba(255,123,123,.14); color: var(--red); border-color: rgba(255,123,123,.2); }
    .badge.info    { background: rgba(152,178,255,.14); color: var(--blue); border-color: rgba(152,178,255,.2); }
    .badge.idle    { background: rgba(141,149,171,.14); color: var(--slate); border-color: rgba(141,149,171,.2); }

    .alert {
      margin-top: 14px;
      border-radius: 18px;
      padding: 14px;
      font-size: 14px;
      font-weight: 700;
      line-height: 1.5;
    }

    .alert.success { background: rgba(110,220,168,.14); color: var(--green); }
    .alert.warning { background: rgba(255,190,107,.14); color: var(--orange); }
    .alert.danger  { background: rgba(255,123,123,.14); color: var(--red); }
    .alert.idle    { background: rgba(141,149,171,.14); color: var(--slate); }

    .actions { display: grid; gap: 10px; }
    .actions.two-col { grid-template-columns: 1fr 1fr; }

    form { margin: 0; }

    .btn {
      width: 100%;
      border: 0;
      border-radius: 18px;
      padding: 14px 16px;
      font-size: 15px;
      font-weight: 800;
      cursor: pointer;
      transition: transform .08s ease, opacity .2s ease;
    }

    .btn:active { transform: translateY(1px); }
    .btn:disabled { opacity: .65; cursor: wait; }

    .btn-primary { background: var(--blue); color: #111; }
    .btn-secondary { background: #2a3143; color: var(--text); }
    .btn-success { background: var(--green); color: #111; }
    .btn-warning { background: var(--orange); color: #111; }
    .btn-danger { background: var(--red); color: #111; }

    .section-title {
      margin: 0 0 14px;
      font-size: 12px;
      color: var(--muted);
      text-transform: uppercase;
      letter-spacing: .2em;
    }

    .stat-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 14px;
    }

    .mini-card {
      background: var(--panel3);
      border: 1px solid var(--line);
      border-radius: 22px;
      padding: 16px;
    }

    .mini-title {
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: .18em;
      color: var(--muted);
      margin-bottom: 10px;
    }

    .mini-value {
      font-size: 28px;
      font-weight: 800;
      line-height: 1;
      margin-bottom: 8px;
    }

    .mini-sub {
      font-size: 14px;
      color: var(--muted);
    }

    .progress-track {
      height: 10px;
      background: rgba(255,255,255,.08);
      border-radius: 999px;
      overflow: hidden;
      margin-top: 14px;
    }

    .progress-fill {
      height: 100%;
      border-radius: 999px;
      background: linear-gradient(90deg, var(--blue), var(--coral));
    }

    .footer-note {
      color: var(--muted);
      font-size: 12px;
      margin-top: 12px;
      text-align: center;
    }

    .note-card {
      border-radius: 18px;
      padding: 14px;
      font-size: 14px;
      line-height: 1.55;
    }

    .note-card.info {
      background: rgba(152,178,255,.14);
      color: var(--blue);
    }

    .note-title {
      font-weight: 800;
      margin-bottom: 6px;
    }

    [dir='rtl'] .header,
    [dir='rtl'] .card-top,
    [dir='rtl'] .row-item {
      direction: rtl;
    }

    [dir='rtl'] .row-main,
    [dir='rtl'] .subtitle,
    [dir='rtl'] .footer-note,
    [dir='rtl'] .note-card,
    [dir='rtl'] .title,
    [dir='rtl'] .card-title {
      text-align: right;
    }

    @media (max-width: 980px) {
      .grid, .hero { grid-template-columns: 1fr; }
    }

    @media (max-width: 640px) {
      body { padding: 10px; }
      .app { border-radius: 24px; }
      .header, .grid { padding-left: 12px; padding-right: 12px; }
      .title { font-size: 24px; }
      .card-title { font-size: 22px; }
      .actions.two-col, .metrics, .stat-grid { grid-template-columns: 1fr; }
      .lang-switch { width: 100%; justify-content: center; }
    }
  </style>
</head>
<body>
  <div class='app'>
    <div class='header'>
      <div>
        <div class='eyebrow' data-ar='مشروع ESP32 الزراعي' data-en='ESP32 Smart Farm Project'></div>
        <h1 class='title' data-ar='لوحة تحكم المزرعة الذكية' data-en='Smart Farm Dashboard'></h1>
        <div class='subtitle' data-ar='مراقبة مستوى الماء، رطوبة التربة، حالة المضخة، ووضع التشغيل.' data-en='Monitor water level, soil moisture, pump state, and operation mode.'></div>
      </div>

      <div class='lang-switch' aria-label='Language switch'>
        <button type='button' class='lang-btn' id='btn-en' onclick='setLang("en")'>EN</button>
        <button type='button' class='lang-btn' id='btn-ar' onclick='setLang("ar")'>AR</button>
      </div>
    </div>

    <div class='grid'>
      <div class='stack'>
        <div class='hero'>
          <div class='card hero-main'>
            <div class='card-top'>
              <div>
                <div class='card-kicker' data-ar='نظرة عامة' data-en='Overview'></div>
                <h2 class='card-title' data-ar='حالة النظام الحالية' data-en='Current System Status'></h2>
              </div>
              <div class='status-pill' data-ar='تحديث مباشر' data-en='Live State'></div>
            </div>

            <div class='metrics'>
              <div class='metric coral'>
                <div class='metric-label' data-ar='مستوى الماء' data-en='Water Level'></div>
                <div class='metric-value'>)rawliteral" + String(waterPercent) + R"rawliteral(%</div>
                <div class='metric-sub'>)rawliteral" + String(waterRaw) + R"rawliteral( RAW</div>
              </div>

              <div class='metric yellow'>
                <div class='metric-label' data-ar='رطوبة التربة' data-en='Soil Moisture'></div>
                <div class='metric-value'>)rawliteral" + (readingDone ? String(soilPercent) + "%" : "--") + R"rawliteral(</div>
                <div class='metric-sub'>)rawliteral" + (readingDone ? String(soilRaw) + " RAW" : "No Data") + R"rawliteral(</div>
              </div>

              <div class='metric lav'>
                <div class='metric-label' data-ar='وضع التشغيل' data-en='Operation Mode'></div>
                <div class='metric-value' data-ar=')rawliteral" + modeAr + R"rawliteral(' data-en=')rawliteral" + modeEn + R"rawliteral('></div>
                <div class='metric-sub' data-ar='تحكم تلقائي أو يدوي' data-en='Automatic or manual control'></div>
              </div>

              <div class='metric blue'>
                <div class='metric-label' data-ar='حالة المضخة' data-en='Pump State'></div>
                <div class='metric-value' data-ar=')rawliteral" + pumpAr + R"rawliteral(' data-en=')rawliteral" + pumpEn + R"rawliteral('></div>
                <div class='metric-sub' data-ar='تشغيل فعلي للري' data-en='Live irrigation state'></div>
              </div>
            </div>
          </div>

          <div class='card'>
            <div class='card-top'>
              <div>
                <div class='card-kicker' data-ar='جاهزية النظام' data-en='System Readiness'></div>
                <h2 class='card-title' data-ar='جاهزية الري' data-en='Irrigation Readiness'></h2>
              </div>
            </div>
            <div class='ring-wrap'>
              <div class='ring'>
                <div class='ring-inner'>
                  <div>
                    <div class='ring-number'>)rawliteral" + String(waterPercent) + R"rawliteral(%</div>
                    <div class='ring-text' data-ar='ماء متاح' data-en='Water Available'></div>
                  </div>
                </div>
              </div>
            </div>
            <div class='footer-note' data-ar='يعتمد تشغيل المضخة على الماء والطاقة ووضع التشغيل.' data-en='Pump operation depends on water, power, and control mode.'></div>
          </div>
        </div>

        <div class='card'>
          <div class='section-title' data-ar='خزان الماء والطاقة' data-en='Water Tank & Power'></div>
          <div class='list'>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='القراءة الخام' data-en='Raw Reading'></div>
                <div class='row-main' id='waterRaw'>)rawliteral" + String(waterRaw) + R"rawliteral(</div>
              </div>
              <div class='badge )rawliteral" + waterState + R"rawliteral(' id='waterStatusBadge' data-ar=')rawliteral" + waterAr + R"rawliteral(' data-en=')rawliteral" + waterEn + R"rawliteral('></div>
            </div>

            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='نسبة الماء' data-en='Water Percentage'></div>
                <div class='row-main' id='waterPercent'>)rawliteral" + String(waterPercent) + R"rawliteral(%</div>
              </div>
              <div class='badge info'>ADC</div>
            </div>

            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='حالة الكهرباء' data-en='Power Status'></div>
                <div class='row-main )rawliteral" + powerState + R"rawliteral(' id='powerStateText' data-ar=')rawliteral" + powerAr + R"rawliteral(' data-en=')rawliteral" + powerEn + R"rawliteral('></div>
              </div>
              <div class='badge )rawliteral" + powerState + R"rawliteral(' id='powerStateBadge' data-ar=')rawliteral" + powerAr + R"rawliteral(' data-en=')rawliteral" + powerEn + R"rawliteral('></div>
            </div>
          </div>

          <div class='alert )rawliteral" + waterState + R"rawliteral(' id='waterAlert' data-ar=')rawliteral" + waterAlertAr + R"rawliteral(' data-en=')rawliteral" + waterAlertEn + R"rawliteral('></div>

          <div class='actions' style='margin-top:14px;'>
            <button id='refreshBtn' class='btn btn-secondary' onclick='refreshWater()' data-ar='تحديث حالة الماء' data-en='Refresh Water'></button>
            <form action='/power' method='POST'>
              <button class='btn )rawliteral" + powerBtnClass + R"rawliteral(' type='submit' data-ar=')rawliteral" + powerBtnAr + R"rawliteral(' data-en=')rawliteral" + powerBtnEn + R"rawliteral('></button>
            </form>
          </div>

          <div class='footer-note'>
            <span data-ar='الحد الأدنى لتشغيل المضخة:' data-en='Minimum water to run pump:'></span>
            )rawliteral" + String(MIN_WATER_PERCENT_TO_RUN) + R"rawliteral(%
          </div>
        </div>

        <div class='card'>
          <div class='section-title' data-ar='حساس التربة' data-en='Soil Sensor'></div>
          <div class='list'>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='القيمة الخام' data-en='Raw Sensor Value'></div>
                <div class='row-main'>)rawliteral" + (readingDone ? String(soilRaw) : "--") + R"rawliteral(</div>
              </div>
              <div class='badge idle'>RAW</div>
            </div>

            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='نسبة الرطوبة' data-en='Moisture Percentage'></div>
                <div class='row-main'>)rawliteral" + (readingDone ? String(soilPercent) + "%" : "--") + R"rawliteral(</div>
              </div>
              <div class='badge info'>ESP32</div>
            </div>

            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='حالة التربة' data-en='Soil Status'></div>
                <div class='row-main )rawliteral" + soilState + R"rawliteral(' data-ar=')rawliteral" + soilAr + R"rawliteral(' data-en=')rawliteral" + soilEn + R"rawliteral('></div>
              </div>
              <div class='badge )rawliteral" + soilState + R"rawliteral(' data-ar=')rawliteral" + soilAr + R"rawliteral(' data-en=')rawliteral" + soilEn + R"rawliteral('></div>
            </div>
          </div>

          <div class='alert )rawliteral" + soilState + R"rawliteral(' data-ar=')rawliteral" + soilAlertAr + R"rawliteral(' data-en=')rawliteral" + soilAlertEn + R"rawliteral('></div>

          <div class='actions' style='margin-top:14px;'>
            <form action='/read' method='POST'>
              <button class='btn btn-primary' type='submit' data-ar='قراءة الحساس' data-en='Read Sensor'></button>
            </form>
          </div>
        </div>
      </div>

      <div class='stack'>
        <div class='card'>
          <div class='section-title' data-ar='المضخة والتحكم' data-en='Pump & Control'></div>

          <div class='stat-grid'>
            <div class='mini-card'>
              <div class='mini-title' data-ar='المضخة' data-en='Pump'></div>
              <div class='mini-value )rawliteral" + pumpState + R"rawliteral(' data-ar=')rawliteral" + pumpAr + R"rawliteral(' data-en=')rawliteral" + pumpEn + R"rawliteral('></div>
              <div class='mini-sub' data-ar='حالة المرحل الحالية' data-en='Current relay state'></div>
            </div>

            <div class='mini-card'>
              <div class='mini-title' data-ar='وضع التشغيل' data-en='Operation Mode'></div>
              <div class='mini-value )rawliteral" + modeState + R"rawliteral(' data-ar=')rawliteral" + modeAr + R"rawliteral(' data-en=')rawliteral" + modeEn + R"rawliteral('></div>
              <div class='mini-sub' data-ar='اختيار يدوي أو تلقائي' data-en='Manual or automatic selection'></div>
            </div>
          </div>

          <div class='list' style='margin-top:14px;'>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='آخر أمر' data-en='Last Command'></div>
                <div class='row-main'>)rawliteral" + lastCmd + R"rawliteral(</div>
              </div>
            </div>
          </div>

          <div class='actions two-col' style='margin-top:14px;'>
            <form action='/mode-auto' method='POST'>
              <button class='btn btn-primary' type='submit' data-ar='وضع تلقائي' data-en='Automatic Mode'></button>
            </form>
            <form action='/mode-manual' method='POST'>
              <button class='btn btn-warning' type='submit' data-ar='وضع يدوي' data-en='Manual Mode'></button>
            </form>
          </div>
        </div>

        <div class='card'>
          <div class='section-title' data-ar='التحكم اليدوي' data-en='Manual Control'></div>
          )rawliteral" + manualSection + R"rawliteral(
        </div>

        <div class='card'>
          <div class='section-title' data-ar='ملخص القرار' data-en='Decision Summary'></div>
          <div class='list'>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='شرط الماء' data-en='Water Condition'></div>
                <div class='row-main' data-ar=')rawliteral" + (waterOK ? "مسموح بالتشغيل" : "محجوب بسبب انخفاض الماء") + R"rawliteral(' data-en=')rawliteral" + (waterOK ? "Allowed to run" : "Blocked by low water") + R"rawliteral('></div>
              </div>
            </div>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='شرط الطاقة' data-en='Power Condition'></div>
                <div class='row-main' data-ar=')rawliteral" + (powerEnabled ? "الطاقة متاحة" : "الطاقة مقطوعة") + R"rawliteral(' data-en=')rawliteral" + (powerEnabled ? "Power available" : "Power disconnected") + R"rawliteral('></div>
              </div>
            </div>
            <div class='row-item'>
              <div class='row-left'>
                <div class='row-label' data-ar='مصدر قرار الري' data-en='Irrigation Decision Source'></div>
                <div class='row-main' data-ar=')rawliteral" + (autoMode ? "قراءة حساس التربة" : "تحكم يدوي") + R"rawliteral(' data-en=')rawliteral" + (autoMode ? "Soil sensor reading" : "Manual control") + R"rawliteral('></div>
              </div>
            </div>
          </div>
          <div class='progress-track'>
            <div class='progress-fill' style='width:)rawliteral" + String(waterPercent) + R"rawliteral(%'></div>
          </div>
          <div class='footer-note' data-ar='هذا المؤشر يعكس نسبة الماء الحالية فقط.' data-en='This indicator reflects the current water percentage only.'></div>
        </div>
      </div>
    </div>
  </div>

  <script>
    function setTextByLang(lang) {
      document.documentElement.lang = lang;
      document.documentElement.dir = lang === 'ar' ? 'rtl' : 'ltr';

      var nodes = document.querySelectorAll('[data-ar][data-en]');
      for (var i = 0; i < nodes.length; i++) {
        nodes[i].innerHTML = (lang === 'ar') ? nodes[i].getAttribute('data-ar') : nodes[i].getAttribute('data-en');
      }

      document.getElementById('btn-ar').classList.toggle('active', lang === 'ar');
      document.getElementById('btn-en').classList.toggle('active', lang === 'en');

      try { localStorage.setItem('farm_lang', lang); } catch(e) {}
    }

    function setLang(lang) {
      setTextByLang(lang);
    }

    function currentLang() {
      try {
        var saved = localStorage.getItem('farm_lang');
        if (saved === 'ar' || saved === 'en') return saved;
      } catch(e) {}
      return 'en';
    }

    function refreshWater() {
      var btn = document.getElementById('refreshBtn');
      var lang = currentLang();

      btn.disabled = true;
      btn.innerHTML = (lang === 'ar') ? 'جاري تحديث الماء...' : 'Refreshing water...';

      fetch('/water-status')
        .then(function(r) { return r.json(); })
        .then(function(data) {
          document.getElementById('waterRaw').innerText = data.raw;
          document.getElementById('waterPercent').innerText = data.percent + '%';

          var statusBadge = document.getElementById('waterStatusBadge');
          var waterAlert = document.getElementById('waterAlert');

          statusBadge.className = 'badge';
          waterAlert.className = 'alert';

          if (data.percent >= 70) {
            statusBadge.classList.add('success');
            waterAlert.classList.add('success');
            statusBadge.setAttribute('data-ar', 'ممتاز');
            statusBadge.setAttribute('data-en', 'Excellent');
            waterAlert.setAttribute('data-ar', 'مستوى الماء ممتاز — جاهز للري');
            waterAlert.setAttribute('data-en', 'Water level is excellent — Ready to irrigate');
          } else if (data.percent >= data.minPercent) {
            statusBadge.classList.add('warning');
            waterAlert.classList.add('warning');
            statusBadge.setAttribute('data-ar', 'متوسط');
            statusBadge.setAttribute('data-en', 'Moderate');
            waterAlert.setAttribute('data-ar', 'مستوى الماء متوسط');
            waterAlert.setAttribute('data-en', 'Water level is moderate');
          } else {
            statusBadge.classList.add('danger');
            waterAlert.classList.add('danger');
            statusBadge.setAttribute('data-ar', 'منخفض');
            statusBadge.setAttribute('data-en', 'Low');
            waterAlert.setAttribute('data-ar', 'الماء منخفض — المضخة محجوبة');
            waterAlert.setAttribute('data-en', 'Water is low — Pump is blocked');
          }

          setTextByLang(lang);
          btn.disabled = false;
          btn.innerHTML = (lang === 'ar') ? 'تحديث حالة الماء' : 'Refresh Water';
        })
        .catch(function() {
          btn.disabled = false;
          btn.innerHTML = (lang === 'ar') ? 'تحديث حالة الماء' : 'Refresh Water';
        });
    }

    setTextByLang(currentLang());
  </script>
</body>
</html>
)rawliteral";

  return page;
}

// ─── Routes / المسارات ─────────────────────────────────────────
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
    pumpOFF("⛔ قُطعت الكهرباء / Power Cut");
  }

  Serial.println(powerEnabled ? "⚡ Power ON" : "⛔ Power OFF");

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleHome() {
  server.send(200, "text/html", buildPage());
}

void handleRead() {
  bool success = fetchSensorReading();

  if (success && autoMode) {
    readWaterLevel();

    if (isDry && !pumpRunning && waterOK) {
      pumpON("Auto — جاف / Dry");
    } else if (!isDry && pumpRunning) {
      pumpOFF("Auto — مبلل / Wet");
    }
  }

  if (!success) {
    Serial.println("❌ Could not reach Sensor ESP32");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOn() {
  if (!autoMode) {
    pumpON("يدوي / Manual ON");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePumpOff() {
  if (!autoMode) {
    pumpOFF("يدوي / Manual OFF");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeAuto() {
  autoMode = true;
  Serial.println("Mode: AUTO");

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleModeManual() {
  autoMode = false;

  if (pumpRunning) {
    pumpOFF("وضع يدوي / Switched to Manual");
  }

  Serial.println("Mode: MANUAL");

  server.sendHeader("Location", "/");
  server.send(303);
}

// ─── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(WATER_PIN, ADC_11db);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ Connected!");
  Serial.print("🌐 Dashboard: http://");
  Serial.println(WiFi.localIP());

  readWaterLevel();
  Serial.print("Initial Water Raw: ");
  Serial.println(waterRaw);
  Serial.print("Initial Water Percent: ");
  Serial.print(waterPercent);
  Serial.println("%");

  server.on("/",             HTTP_GET,  handleHome);
  server.on("/water-status", HTTP_GET,  handleWaterStatus);
  server.on("/power",        HTTP_POST, handlePowerToggle);
  server.on("/read",         HTTP_POST, handleRead);
  server.on("/on",           HTTP_POST, handlePumpOn);
  server.on("/off",          HTTP_POST, handlePumpOff);
  server.on("/mode-auto",    HTTP_POST, handleModeAuto);
  server.on("/mode-manual",  HTTP_POST, handleModeManual);

  server.begin();
}

// ─── Loop ──────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (pumpRunning && (millis() - pumpStart >= PUMP_DURATION)) {
    pumpOFF("إيقاف تلقائي / Auto Stop — Timer");
  }

  if (pumpRunning) {
    readWaterLevel();

    if (!waterOK) {
      pumpOFF("⛔ الماء منخفض / Water Too Low");
      Serial.println("⚠️ Water level dropped too low — pump stopped immediately!");
    }
  }
}
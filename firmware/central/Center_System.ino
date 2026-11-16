/*
  Smart Parking - Central Controller (ESP32 30-pin)
  Combined: QoS experiment + push button keluar + servo keluar + animasi TFT

  PIN MAPPING ESP32:
  GPIO5  - CS TFT
  GPIO4  - RST TFT
  GPIO2  - DC TFT
  GPIO18 - SCK TFT  (VSPI SCK)
  GPIO23 - MOSI TFT (VSPI MOSI)
  GPIO25 - Servo masuk
  GPIO26 - Servo keluar
  GPIO34 - IR sensor gerbang masuk (input only)
  GPIO35 - Push button keluar      (input only, pasang resistor 10K ke 3.3V)
  GPIO21 - SDA LCD I2C
  GPIO22 - SCL LCD I2C
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "secrets.h"

// ============================================================
// BUILD SETTINGS
// ============================================================
#define USE_HTTP        0
#define PUBLISH_INTERVAL 5000UL
#define EXPERIMENT_MODE  1

#if USE_HTTP != 0 && USE_HTTP != 1
  #error "USE_HTTP must be 0 or 1."
#endif

// ============================================================
// PIN ESP32
// ============================================================
#define TFT_CS           5
#define TFT_RST          4
#define TFT_DC           2
#define TFT_SCK         18   // VSPI SCK
#define TFT_MOSI        23   // VSPI MOSI
#define SERVO_MASUK_PIN 25
#define SERVO_KELUAR_PIN 26
#define IR_GATE_PIN     34   // input only, tidak perlu pullup
#define BTN_KELUAR_PIN  33   // input only, pasang resistor 10K eksternal ke 3.3V

#define SERVO_BUKA   90
#define SERVO_TUTUP   0
#define DURASI_PALANG_KELUAR 5000UL

// ============================================================
// NETWORK
// ============================================================
#define TB_PORT_MQTT    8883
#define LOCAL_MQTT_PORT 1883
#define TG_COOLDOWN   10000UL

char mqtt_local[40] = "";
WiFiManager wm;
WiFiManagerParameter custom_mqtt_local("mqtt_local", "IP Broker Mosquitto (laptop)", "", 40);

// ESP32 pakai Preferences sebagai pengganti EEPROM
Preferences prefs;

void saveIPToPrefs(const char* ip) {
  prefs.begin("parking", false);
  prefs.putString("mqtt_local", ip);
  prefs.end();
  Serial.printf("[Prefs] IP broker disimpan: %s\n", ip);
}

void loadIPFromPrefs(char* ip) {
  prefs.begin("parking", true);
  String val = prefs.getString("mqtt_local", "");
  prefs.end();
  strlcpy(ip, val.c_str(), 40);
  Serial.printf("[Prefs] IP broker dimuat: %s\n", ip);
}

// ============================================================
// OBJEK
// ============================================================
// ESP32 VSPI: SCK=18, MOSI=23, MISO=19 (tidak dipakai TFT)
SPIClass spiTFT(VSPI);
Adafruit_ST7735 display = Adafruit_ST7735(&spiTFT, TFT_CS, TFT_DC, TFT_RST);

Servo servoMasuk;
Servo servoKeluar;
LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClientSecure tbSecureClient;
WiFiClient       localClient;
PubSubClient     tbMqtt(tbSecureClient);
PubSubClient     localMqtt(localClient);
HTTPClient       tbHttp;

const char* topic_nodeA = "parkir/nodeA/status";
const char* topic_nodeB = "parkir/nodeB/status";
const char* topic_nodeC = "parkir/nodeC/status";
const char* TB_TOPIC    = "v1/devices/me/telemetry";

// ============================================================
// PARKING STATE
// ============================================================
String statusBlok[3]    = {"UNKNOWN", "UNKNOWN", "UNKNOWN"};
int    slotKosong[3]    = {0, 0, 0};
String namaBlok[3]      = {"A", "B", "C"};
bool   isRecommended[3] = {false, false, false};
unsigned long lastNodeSeen[3] = {0, 0, 0};

String statusKendaraan     = "TIDAK_ADA";
String lastStatusKendaraan = "";
bool   palangMasukTerbuka  = false;
String lastStatusKeseluruhan = "";

// ============================================================
// PALANG KELUAR
// ============================================================
bool          palangKeluarTerbuka = false;
unsigned long waktuBukaKeluar     = 0;
bool          lastBtnState        = HIGH;

// ============================================================
// EXPERIMENT STATE
// ============================================================
uint32_t cloudSeq       = 0;
uint32_t cloudAttempts  = 0;
uint32_t warmupAttempts = 0;
bool     clockReady     = false;

// ============================================================
// ANIMASI TFT
// ============================================================
unsigned long previousBlinkMillis = 0;
unsigned long previousAnimMillis  = 0;
bool blinkState     = false;
int  animOffset     = 0;
bool forceRedrawTFT = true;

// ============================================================
// TIMER UMUM
// ============================================================
unsigned long previousPublish    = 0;
unsigned long lastLocalReconnect = 0;
unsigned long lastTelegramSend   = 0;

// ============================================================
// TIME / NTP
// ============================================================
bool syncClock(uint32_t timeoutMs = 20000) {
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.printf("[NTP] Synchronized: %lu\n", (unsigned long)now);
      return true;
    }
    delay(250);
  }
  Serial.println("[NTP] Sync timeout.");
  return false;
}

uint64_t epochMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// ============================================================
// WIFIMANAGER CALLBACK
// ============================================================
void saveConfigCallback() {
  const char* newIp = custom_mqtt_local.getValue();
  if (newIp && strlen(newIp) > 0) {
    strlcpy(mqtt_local, newIp, sizeof(mqtt_local));
    saveIPToPrefs(mqtt_local);
  }
}

// ============================================================
// TELEGRAM
// ============================================================
void kirimTelegram(const String& pesan) {
#if ENABLE_TELEGRAM_DURING_QOS_TEST == 0
  if (EXPERIMENT_MODE) return;
#endif
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTelegramSend < TG_COOLDOWN) return;
  lastTelegramSend = millis();

  WiFiClientSecure tgClient;
  tgClient.setInsecure();
  HTTPClient http;

  String url  = String("https://api.telegram.org/bot") + TG_BOT_TOKEN + "/sendMessage";
  String body = "{\"chat_id\":\"" + String(TG_CHAT_ID) +
                "\",\"text\":\"" + pesan +
                "\",\"parse_mode\":\"HTML\"}";

  if (http.begin(tgClient, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    Serial.printf("[Telegram] code=%d\n", code);
    http.end();
  }
}

// ============================================================
// LOGIKA PARKIR
// ============================================================
int totalSlotKosong() {
  return slotKosong[0] + slotKosong[1] + slotKosong[2];
}

void hitungRekomendasi() {
  for (int i = 0; i < 3; i++) isRecommended[i] = false;
  int count = 0;
  for (int i = 0; i < 3 && count < 2; i++) {
    if (statusBlok[i] == "TERSEDIA") {
      isRecommended[i] = true;
      count++;
    }
  }
}

String getRekomendasiStr() {
  String hasil = "";
  int count = 0;
  for (int i = 0; i < 3 && count < 2; i++) {
    if (isRecommended[i]) {
      if (count > 0) hasil += " & ";
      hasil += "Blok " + namaBlok[i];
      count++;
    }
  }
  return hasil.length() ? hasil : "PENUH / UNKNOWN";
}

// ============================================================
// KENDALI PALANG MASUK
// ============================================================
void kendaliPalangMasuk() {
  int total    = totalSlotKosong();
  bool wasOpen = palangMasukTerbuka;

  if (statusKendaraan == "ADA" && total > 0 && !palangMasukTerbuka) {
    servoMasuk.write(SERVO_BUKA);
    palangMasukTerbuka = true;
  } else if ((statusKendaraan == "TIDAK_ADA" || total == 0) && palangMasukTerbuka) {
    servoMasuk.write(SERVO_TUTUP);
    palangMasukTerbuka = false;
  }

  if (palangMasukTerbuka != wasOpen) {
    String msg = palangMasukTerbuka
      ? "Palang Masuk TERBUKA; slot tersedia: " + String(total)
      : "Palang Masuk TERTUTUP; slot kosong: " + String(total);
    kirimTelegram(msg);
  }
}

// ============================================================
// KENDALI PALANG KELUAR
// ============================================================
void bukaPalangKeluar() {
  if (palangKeluarTerbuka) return;
  servoKeluar.write(SERVO_BUKA);
  palangKeluarTerbuka = true;
  waktuBukaKeluar     = millis();
  Serial.println("[Palang Keluar] BUKA");
  kirimTelegram("Palang Keluar TERBUKA; petugas membuka palang keluar.");
}

void tutupPalangKeluar() {
  servoKeluar.write(SERVO_TUTUP);
  palangKeluarTerbuka = false;
  Serial.println("[Palang Keluar] TUTUP");
}

// ============================================================
// CEK PUSH BUTTON KELUAR
// GPIO35 adalah input-only, tidak ada internal pullup
// Pasang resistor 10K dari GPIO35 ke 3.3V (pull-up eksternal)
// Tombol: GPIO35 → GND
// ============================================================
void cekTombolKeluar() {
  bool btnState = digitalRead(BTN_KELUAR_PIN);

  // tombol ditekan (HIGH → LOW) → buka palang
  if (btnState == LOW && lastBtnState == HIGH) {
    delay(50); // debounce
    if (digitalRead(BTN_KELUAR_PIN) == LOW) {
      servoKeluar.write(SERVO_BUKA);
      palangKeluarTerbuka = true;
      Serial.println("[Palang Keluar] BUKA");
      kirimTelegram("Palang Keluar TERBUKA; petugas menahan tombol.");
    }
  }

  // tombol dilepas (LOW → HIGH) → tutup palang
  if (btnState == HIGH && lastBtnState == LOW) {
    delay(50); // debounce
    if (digitalRead(BTN_KELUAR_PIN) == HIGH) {
      servoKeluar.write(SERVO_TUTUP);
      palangKeluarTerbuka = false;
      Serial.println("[Palang Keluar] TUTUP");
    }
  }

  lastBtnState = btnState;
}

// ============================================================
// DISPLAY — LCD
// ============================================================
void updateLCD() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Kondisi Parkir:");
  lcd.setCursor(0, 1);
  if (!localMqtt.connected()) lcd.print("Menunggu MQTT...");
  else if (totalSlotKosong() == 0) lcd.print("PENUH (0 Slot)");
  else {
    lcd.print("Tersedia: ");
    lcd.print(totalSlotKosong());
    lcd.print(" Slot");
  }
}

// ============================================================
// DISPLAY — TFT
// ============================================================
void drawBlock(int idx, int x, int y) {
  uint16_t boxColor, textColor;
  bool fillBox = false;

  if (statusBlok[idx] == "UNKNOWN") {
    boxColor = ST7735_YELLOW; textColor = ST7735_BLACK; fillBox = true;
  } else if (statusBlok[idx] == "PENUH") {
    boxColor = ST7735_RED; textColor = ST7735_WHITE; fillBox = true;
  } else {
    if (isRecommended[idx]) {
      boxColor  = ST7735_GREEN;
      textColor = blinkState ? ST7735_BLACK : ST7735_GREEN;
      fillBox   = blinkState;
    } else {
      boxColor = ST7735_BLUE; textColor = ST7735_WHITE;
    }
  }

  if (fillBox) display.fillRect(x, y, 70, 20, boxColor);
  else {
    display.fillRect(x, y, 70, 20, ST7735_BLACK);
    display.drawRect(x, y, 70, 20, boxColor);
  }
  display.setTextColor(textColor);
  display.setCursor(x + 5, y + 6);
  display.print("BLOK ");
  display.print(namaBlok[idx]);
  display.print(" ");
  display.print(slotKosong[idx]);
  display.print("/3");
}

void updateTFT() {
  if (forceRedrawTFT) {
    display.fillScreen(ST7735_BLACK);
    for (int x = 5; x < 160; x += 12)
      display.drawLine(x, 40, x + 6, 40, ST7735_WHITE);
    forceRedrawTFT = false;
  }
  drawBlock(0,  5,  5);
  drawBlock(1, 85,  5);
  drawBlock(2, 45, 55);
}

void drawPathLine(int target, int d1, int d2, int turn_x, int path_y) {
  if (d1 == d2) return;
  if (d1 <= turn_x && d2 <= turn_x) {
    display.fillRect(d1, path_y, d2 - d1, 3, ST7735_GREEN);
  } else {
    int v_start = d1 - turn_x, v_end = d2 - turn_x, len = v_end - v_start;
    if (target == 0 || target == 1)
      display.fillRect(turn_x, path_y - v_end, 3, len, ST7735_GREEN);
    else
      display.fillRect(turn_x, path_y + v_start, 3, len, ST7735_GREEN);
  }
}

void drawAnimatedPath(int target, int offset) {
  int turn_x = 80;
  if (target == 0) turn_x = 40;
  else if (target == 1) turn_x = 120;

  int path_y    = 35;
  int vert_len  = (target == 0 || target == 1) ? 5 : 15;
  int total_len = turn_x + vert_len;

  display.fillRect(0, path_y, turn_x + 3, 3, ST7735_BLACK);
  if (target == 0 || target == 1) {
    display.fillRect(turn_x, 30, 3, 5, ST7735_BLACK);
  } else {
    display.fillRect(turn_x, 38, 3, 12, ST7735_BLACK);
    display.drawLine(turn_x - 5, 40, turn_x + 5, 40, ST7735_WHITE);
  }

  if (target == 0 || target == 1)
    display.fillTriangle(turn_x - 3, 30, turn_x + 5, 30, turn_x + 1, 25, ST7735_GREEN);
  else
    display.fillTriangle(turn_x - 3, 50, turn_x + 5, 50, turn_x + 1, 55, ST7735_GREEN);

  int period = 14, dash = 10;
  for (int i = -1; i <= (total_len / period) + 1; i++) {
    int d_start = i * period + offset, d_end = d_start + dash;
    if (d_end < 0 || d_start > total_len) continue;
    d_start = max(0, d_start); d_end = min(total_len, d_end);
    if (d_start <= turn_x && d_end > turn_x) {
      drawPathLine(target, d_start, turn_x, turn_x, path_y);
      drawPathLine(target, turn_x,  d_end,  turn_x, path_y);
    } else {
      drawPathLine(target, d_start, d_end, turn_x, path_y);
    }
  }
}

void updateNavigationAnimation() {
  int targetBlok = -1;
  for (int i = 0; i < 3; i++) {
    if (isRecommended[i]) { targetBlok = i; break; }
  }
  if (targetBlok != -1) {
    drawAnimatedPath(targetBlok, animOffset);
  } else {
    display.fillRect(0, 25, 160, 30, ST7735_BLACK);
    for (int x = 5; x < 160; x += 12)
      display.drawLine(x, 40, x + 6, 40, ST7735_WHITE);
  }
}

// ============================================================
// STALE NODE DETECTION
// ============================================================
void markStaleNodes() {
  const unsigned long NODE_TIMEOUT_MS = 15000UL;
  bool changed = false;
  for (int i = 0; i < 3; i++) {
    if (lastNodeSeen[i] != 0 &&
        millis() - lastNodeSeen[i] > NODE_TIMEOUT_MS &&
        statusBlok[i] != "UNKNOWN") {
      statusBlok[i] = "UNKNOWN";
      slotKosong[i] = 0;
      changed = true;
    }
  }
  if (changed) {
    hitungRekomendasi();
    updateLCD();
    forceRedrawTFT = true;
  }
}

// ============================================================
// LOCAL MQTT CALLBACK
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char payloadChar[384];
  int len = (length < sizeof(payloadChar) - 1) ? (int)length : (int)sizeof(payloadChar) - 1;
  memcpy(payloadChar, payload, len);
  payloadChar[len] = '\0';

  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, payloadChar)) return;

  String topicStr(topic);
  String nodeId = doc["node_id"] | "";
  int idx = -1;
  if      (topicStr == topic_nodeA && nodeId == "A") idx = 0;
  else if (topicStr == topic_nodeB && nodeId == "B") idx = 1;
  else if (topicStr == topic_nodeC && nodeId == "C") idx = 2;
  if (idx < 0) return;

  String blok = doc["blok"] | "UNKNOWN";
  int kosong  = doc["slot_kosong"] | 0;
  if (kosong < 0) kosong = 0;
  if (kosong > 3) kosong = 3;

  statusBlok[idx]   = blok;
  slotKosong[idx]   = kosong;
  lastNodeSeen[idx] = millis();

  hitungRekomendasi();
  kendaliPalangMasuk();
  updateLCD();
  forceRedrawTFT = true;

  String current = "";
  for (int i = 0; i < 3; i++)
    current += statusBlok[i] + "|" + String(slotKosong[i]) + ";";
  if (current != lastStatusKeseluruhan) {
    lastStatusKeseluruhan = current;
    String msg = "Update Parkir. Kosong: " + String(totalSlotKosong()) +
                 ". Rekomendasi: " + getRekomendasiStr();
    kirimTelegram(msg);
  }
}

// ============================================================
// RECONNECT
// ============================================================
void reconnectLocal() {
  if (localMqtt.connected() || strlen(mqtt_local) == 0) return;
  if (millis() - lastLocalReconnect < 5000UL) return;
  lastLocalReconnect = millis();
  if (localMqtt.connect("SP-LocalSub")) {
    localMqtt.subscribe(topic_nodeA);
    localMqtt.subscribe(topic_nodeB);
    localMqtt.subscribe(topic_nodeC);
    Serial.println("[LocalMQTT] connected");
    updateLCD();
  } else {
    Serial.printf("[LocalMQTT] failed, rc=%d\n", localMqtt.state());
  }
}

void reconnectTB() {
#if USE_HTTP == 0
  if (tbMqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (tbMqtt.connect("SP-ThingsBoard", TB_TOKEN_CENTER, "")) {
    Serial.println("[TB-MQTT] connected");
  } else {
    Serial.printf("[TB-MQTT] failed, rc=%d\n", tbMqtt.state());
  }
#endif
}

// ============================================================
// PUBLISH KE THINGSBOARD
// ============================================================
bool publishMQTT(const char* jsonBuf, uint32_t seq, uint64_t txMs) {
  size_t payloadBytes = strlen(jsonBuf);
  if (!tbMqtt.connected()) {
    Serial.printf("CLOUD_LOG,%s,CENTER,MQTT,%lu,%llu,NOT_CONNECTED,%u,0\n",
                  EXPERIMENT_RUN_ID, (unsigned long)seq,
                  (unsigned long long)txMs, (unsigned)payloadBytes);
    return false;
  }
  unsigned long t0 = micros();
  bool ok = tbMqtt.publish(TB_TOPIC, jsonBuf);
  unsigned long callUs = micros() - t0;
  Serial.printf("CLOUD_LOG,%s,CENTER,MQTT,%lu,%llu,%s,%u,%lu\n",
                EXPERIMENT_RUN_ID, (unsigned long)seq,
                (unsigned long long)txMs,
                ok ? "CLIENT_SENT_QOS0" : "CLIENT_FAILED",
                (unsigned)payloadBytes, callUs);
  return ok;
}

bool publishHTTP(const char* jsonBuf, uint32_t seq, uint64_t txMs) {
  size_t payloadBytes = strlen(jsonBuf);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("CLOUD_LOG,%s,CENTER,HTTP,%lu,%llu,NO_WIFI,%u,0\n",
                  EXPERIMENT_RUN_ID, (unsigned long)seq,
                  (unsigned long long)txMs, (unsigned)payloadBytes);
    return false;
  }
  String url = String("https://") + TB_HOST + "/api/v1/" + TB_TOKEN_CENTER + "/telemetry";
  if (!tbHttp.begin(tbSecureClient, url)) return false;
  tbHttp.setReuse(true);
  tbHttp.setTimeout(8000);
  tbHttp.addHeader("Content-Type", "application/json", false, true);
  unsigned long t0 = micros();
  int code = tbHttp.POST((uint8_t*)jsonBuf, strlen(jsonBuf));
  unsigned long rttUs = micros() - t0;
  bool ok = (code >= 200 && code < 300);
  Serial.printf("CLOUD_LOG,%s,CENTER,HTTP,%lu,%llu,%d,%u,%lu\n",
                EXPERIMENT_RUN_ID, (unsigned long)seq,
                (unsigned long long)txMs, code,
                (unsigned)payloadBytes, rttUs);
  tbHttp.end();
  return ok;
}

bool sendToThingsBoard(const char* jsonBuf, uint32_t seq, uint64_t txMs) {
#if USE_HTTP
  return publishHTTP(jsonBuf, seq, txMs);
#else
  return publishMQTT(jsonBuf, seq, txMs);
#endif
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // pin
  pinMode(IR_GATE_PIN, INPUT);
  pinMode(BTN_KELUAR_PIN, INPUT_PULLUP); // input only, tidak ada pullup internal

  // servo — ESP32Servo perlu alokasi timer
  servoMasuk.attach(SERVO_MASUK_PIN);
  servoMasuk.write(SERVO_TUTUP);
  servoKeluar.attach(SERVO_KELUAR_PIN);
  servoKeluar.write(SERVO_TUTUP);

  // LCD I2C (GPIO21=SDA, GPIO22=SCL — default ESP32)
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Memulai sistem..");

  // TFT via VSPI (SCK=18, MOSI=23)
  spiTFT.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  display.initR(INITR_MINI160x80);
  display.setRotation(1);
  display.fillScreen(ST7735_BLACK);
  display.setTextColor(ST7735_WHITE);
  display.setCursor(10, 35);
  display.print("Konek WiFi...");

  // load IP broker
  loadIPFromPrefs(mqtt_local);

  // WiFiManager
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&custom_mqtt_local);
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("QoS-SistemPusat", WIFI_AP_PASSWORD)) {
    Serial.println("WiFi failed; restarting.");
    delay(2000);
    ESP.restart();
  }

  const char* enteredIp = custom_mqtt_local.getValue();
  if (enteredIp && strlen(enteredIp) > 0)
    strlcpy(mqtt_local, enteredIp, sizeof(mqtt_local));

  Serial.printf("WiFi connected. IP=%s, Mosquitto=%s\n",
                WiFi.localIP().toString().c_str(), mqtt_local);

  // NTP
  clockReady = syncClock();

  // ThingsBoard
  tbSecureClient.setInsecure();
  tbMqtt.setServer(TB_HOST, TB_PORT_MQTT);
  tbMqtt.setKeepAlive(30);
  tbMqtt.setSocketTimeout(8);
  tbMqtt.setBufferSize(512);

  // broker lokal
  localMqtt.setServer(mqtt_local, LOCAL_MQTT_PORT);
  localMqtt.setCallback(mqttCallback);
  localMqtt.setKeepAlive(30);
  localMqtt.setBufferSize(512);

  hitungRekomendasi();
  updateLCD();
  updateTFT();

  reconnectLocal();
#if USE_HTTP == 0
  reconnectTB();
#endif

  Serial.println("LOG_HEADER,run_id,device,protocol,seq,tx_epoch_ms,result,payload_bytes,duration_us");
  Serial.printf("READY,CENTER,protocol=%s,run=%s,clock=%s\n",
                USE_HTTP ? "HTTP" : "MQTT",
                EXPERIMENT_RUN_ID,
                clockReady ? "OK" : "NOT_READY");

  kirimTelegram("Sistem Pusat Online (ESP32)");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) { delay(10); return; }

  unsigned long now = millis();

  // 1. koneksi
  reconnectLocal();
  if (localMqtt.connected()) localMqtt.loop();
#if USE_HTTP == 0
  reconnectTB();
  if (tbMqtt.connected()) tbMqtt.loop();
#endif

  // 2. sensor gerbang masuk (debounce)
  String gateNow = (digitalRead(IR_GATE_PIN) == LOW) ? "ADA" : "TIDAK_ADA";
  if (gateNow != lastStatusKendaraan) {
    lastStatusKendaraan = gateNow;
    statusKendaraan     = gateNow;
    kendaliPalangMasuk();
  }

  // 3. push button keluar
  cekTombolKeluar();

  // // 4. tutup palang keluar otomatis setelah 5 detik
  // if (palangKeluarTerbuka && (now - waktuBukaKeluar >= DURASI_PALANG_KELUAR)) {
  //   tutupPalangKeluar();
  // }

  // 5. stale node detection
  markStaleNodes();

  // 6. publish ke ThingsBoard tiap 5 detik
  if (now - previousPublish >= PUBLISH_INTERVAL) {
    previousPublish = now;

    bool limitReached = (MAX_CLOUD_MESSAGES > 0 && cloudAttempts >= MAX_CLOUD_MESSAGES);
    bool isWarmup     = (warmupAttempts < WARMUP_CLOUD_MESSAGES);

    if ((!limitReached || isWarmup) && clockReady) {
#if USE_HTTP == 0
      if (!tbMqtt.connected()) {
        Serial.println("[TB-MQTT] disconnected before timestamp; interval skipped.");
      } else
#endif
      {
        uint32_t seq  = cloudSeq++;
        uint64_t txMs = epochMillis();

        if (txMs > 0) {
          StaticJsonDocument<512> doc;
          doc["total_slot_kosong"] = totalSlotKosong();
          doc["status_blok_A"]     = statusBlok[0];
          doc["status_blok_B"]     = statusBlok[1];
          doc["status_blok_C"]     = statusBlok[2];
          doc["slot_kosong_A"]     = slotKosong[0];
          doc["slot_kosong_B"]     = slotKosong[1];
          doc["slot_kosong_C"]     = slotKosong[2];
          doc["kendaraan"]         = statusKendaraan;
          doc["palang_masuk"]      = palangMasukTerbuka  ? "TERBUKA" : "TERTUTUP";
          doc["palang_keluar"]     = palangKeluarTerbuka ? "TERBUKA" : "TERTUTUP";
          doc["rekomendasi"]       = getRekomendasiStr();
          doc["run_id"]            = EXPERIMENT_RUN_ID;
          doc["seq"]               = seq;
          doc["send_timestamp"]    = (unsigned long long)txMs;
          doc["protokol"]          = USE_HTTP ? "HTTP" : "MQTT";
          doc["phase"]             = isWarmup ? "WARMUP" : "MEASURE";

          char jsonBuf[512];
          size_t len = serializeJson(doc, jsonBuf, sizeof(jsonBuf));
          if (len > 0) {
            if (isWarmup) warmupAttempts++;
            else cloudAttempts++;
            sendToThingsBoard(jsonBuf, seq, txMs);
          }
        } else {
          Serial.println("[TIME] Invalid epoch; cloud sample skipped.");
        }
      }
    }
  }

  // 7. animasi TFT kedip blok tiap 500ms
  if (now - previousBlinkMillis >= 500) {
    previousBlinkMillis = now;
    blinkState = !blinkState;
    updateTFT();
  }

  // 8. animasi navigasi tiap 100ms
  if (now - previousAnimMillis >= 100) {
    previousAnimMillis = now;
    animOffset = (animOffset + 2) % 14;
    updateNavigationAnimation();
  }
}
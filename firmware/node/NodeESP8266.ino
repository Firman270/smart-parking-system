/*
  Smart Parking - Node A/B/C (ESP8266)
  Combined: QoS experiment + fake sensor switch per slot

  BEFORE UPLOAD:
  1. Buat secrets.h dari secrets.h.example
  2. Set NODE_INDEX: 0=A, 1=B, 2=C
  3. Set USE_HTTP sama di semua board
  4. Set USE_FAKE_SENSOR per slot sesuai kondisi hardware
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "secrets.h"

// ============================================================
// BUILD SETTINGS — GANTI SEBELUM UPLOAD
// ============================================================
#define NODE_INDEX 2        // 0=A, 1=B, 2=C
#define USE_HTTP   0        // 0=MQTT/TLS, 1=HTTPS — sama di semua board
#define PUBLISH_INTERVAL 5000UL
#define EXPERIMENT_MODE  1

// ============================================================
// FAKE SENSOR — per slot
// 0 = pakai sensor IR asli
// 1 = default TERISI (simulasi slot tidak ada sensor)
// ============================================================
#define USE_FAKE_SENSOR_SLOT1 0
#define USE_FAKE_SENSOR_SLOT2 0
#define USE_FAKE_SENSOR_SLOT3 0

// ============================================================
// NODE ID & TOKEN
// ============================================================
#if NODE_INDEX == 0
  #define NODE_ID        "A"
  #define NODE_TOKEN     TB_TOKEN_NODE_A
  #define AP_PORTAL_NAME "QoS-Node-A"
#elif NODE_INDEX == 1
  #define NODE_ID        "B"
  #define NODE_TOKEN     TB_TOKEN_NODE_B
  #define AP_PORTAL_NAME "QoS-Node-B"
#elif NODE_INDEX == 2
  #define NODE_ID        "C"
  #define NODE_TOKEN     TB_TOKEN_NODE_C
  #define AP_PORTAL_NAME "QoS-Node-C"
#else
  #error "NODE_INDEX must be 0, 1, or 2."
#endif

#if USE_HTTP != 0 && USE_HTTP != 1
  #error "USE_HTTP must be 0 or 1."
#endif

// ============================================================
// PIN SENSOR IR
// D3 (GPIO0) dihindari karena boot-sensitive
// ============================================================
#define IR_SLOT1 D1   // GPIO5
#define IR_SLOT2 D2   // GPIO4
#define IR_SLOT3 D5   // GPIO14

// ============================================================
// NETWORK
// ============================================================
#define TB_PORT_MQTT    8883
#define LOCAL_MQTT_PORT 1883
#define EEPROM_SIZE      64
#define EEPROM_ADDR_IP    0
#define TG_COOLDOWN   30000UL

char mqtt_local[40] = "";
WiFiManager wm;
WiFiManagerParameter custom_mqtt_local("mqtt_local", "IP Broker Mosquitto (laptop)", "", 40);

WiFiClientSecure tbSecureClient;
WiFiClient       localWifiClient;
PubSubClient     tbClient(tbSecureClient);
PubSubClient     localMqtt(localWifiClient);
HTTPClient       tbHttp;

const char* TB_TOPIC          = "v1/devices/me/telemetry";
const char* LOCAL_TOPIC_PREFIX = "parkir/node";

// ============================================================
// EXPERIMENT STATE
// ============================================================
uint32_t      cloudSeq       = 0;
uint32_t      cloudAttempts  = 0;
uint32_t      warmupAttempts = 0;
uint32_t      localSeq       = 0;
unsigned long lastPublish         = 0;
unsigned long lastLocalReconnect  = 0;
unsigned long lastTelegramSend    = 0;
bool          clockReady          = false;
String        lastStatusBlok      = "";

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
    yield();
  }
  Serial.println("[NTP] Sync timeout; cloud publish paused.");
  return false;
}

uint64_t epochMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// ============================================================
// EEPROM
// ============================================================
void saveIPToEEPROM(const char* ip) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 40; i++)
    EEPROM.write(EEPROM_ADDR_IP + i, (i < (int)strlen(ip)) ? ip[i] : 0);
  EEPROM.commit();
  EEPROM.end();
}

void loadIPFromEEPROM(char* ip) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 40; i++)
    ip[i] = (char)EEPROM.read(EEPROM_ADDR_IP + i);
  ip[39] = '\0';
  EEPROM.end();
}

void saveConfigCallback() {
  const char* newIp = custom_mqtt_local.getValue();
  if (newIp && strlen(newIp) > 0) {
    strlcpy(mqtt_local, newIp, sizeof(mqtt_local));
    saveIPToEEPROM(mqtt_local);
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
  tgClient.setBufferSizes(512, 512);
  tgClient.setTimeout(5000);
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
// SENSOR
// IR obstacle: LOW = ada objek (TERISI), HIGH = tidak ada (KOSONG)
// Fake sensor: selalu kembalikan TERISI
// ============================================================
String bacaSensor(int pin, int useFake) {
  if (useFake == 1) return "TERISI";
  return (digitalRead(pin) == LOW) ? "TERISI" : "KOSONG";
}

String hitungStatusBlok(const String& s1, const String& s2, const String& s3) {
  return (s1 == "KOSONG" || s2 == "KOSONG" || s3 == "KOSONG") ? "TERSEDIA" : "PENUH";
}

// ============================================================
// LOCAL MQTT
// ============================================================
void reconnectLocal() {
  if (localMqtt.connected() || strlen(mqtt_local) == 0) return;
  if (millis() - lastLocalReconnect < 5000UL) return;
  lastLocalReconnect = millis();
  String clientId = String("Local_Node") + NODE_ID;
  if (localMqtt.connect(clientId.c_str())) {
    Serial.println("[LocalMQTT] connected");
  } else {
    Serial.printf("[LocalMQTT] failed, rc=%d\n", localMqtt.state());
  }
}

bool publishLokal(const char* topic, const char* jsonBuf) {
  reconnectLocal();
  if (!localMqtt.connected()) return false;
  bool ok = localMqtt.publish(topic, jsonBuf);
  Serial.printf("LOCAL_MQTT,%s,%lu,%s\n", NODE_ID, (unsigned long)localSeq, ok ? "QUEUED" : "FAILED");
  return ok;
}

// ============================================================
// THINGSBOARD
// ============================================================
void reconnectTB() {
#if USE_HTTP == 0
  if (tbClient.connected() || WiFi.status() != WL_CONNECTED) return;
  String clientId = String("TB_Node") + NODE_ID;
  if (tbClient.connect(clientId.c_str(), NODE_TOKEN, "")) {
    Serial.println("[TB-MQTT] connected");
  } else {
    Serial.printf("[TB-MQTT] failed, rc=%d\n", tbClient.state());
  }
#endif
}

bool publishMQTT(const char* jsonBuf, uint32_t seq, uint64_t txMs) {
  size_t payloadBytes = strlen(jsonBuf);
  if (!tbClient.connected()) {
    Serial.printf("CLOUD_LOG,%s,%s,MQTT,%lu,%llu,NOT_CONNECTED,%u,0\n",
                  EXPERIMENT_RUN_ID, NODE_ID,
                  (unsigned long)seq, (unsigned long long)txMs,
                  (unsigned)payloadBytes);
    return false;
  }
  unsigned long t0 = micros();
  bool ok = tbClient.publish(TB_TOPIC, jsonBuf);
  unsigned long callUs = micros() - t0;
  Serial.printf("CLOUD_LOG,%s,%s,MQTT,%lu,%llu,%s,%u,%lu\n",
                EXPERIMENT_RUN_ID, NODE_ID,
                (unsigned long)seq, (unsigned long long)txMs,
                ok ? "CLIENT_SENT_QOS0" : "CLIENT_FAILED",
                (unsigned)payloadBytes, callUs);
  return ok;
}

bool publishHTTP(const char* jsonBuf, uint32_t seq, uint64_t txMs) {
  size_t payloadBytes = strlen(jsonBuf);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("CLOUD_LOG,%s,%s,HTTP,%lu,%llu,NO_WIFI,%u,0\n",
                  EXPERIMENT_RUN_ID, NODE_ID,
                  (unsigned long)seq, (unsigned long long)txMs,
                  (unsigned)payloadBytes);
    return false;
  }
  String url = String("https://") + TB_HOST + "/api/v1/" + NODE_TOKEN + "/telemetry";
  if (!tbHttp.begin(tbSecureClient, url)) {
    Serial.printf("CLOUD_LOG,%s,%s,HTTP,%lu,%llu,BEGIN_FAILED,0\n",
                  EXPERIMENT_RUN_ID, NODE_ID,
                  (unsigned long)seq, (unsigned long long)txMs);
    return false;
  }
  tbHttp.setReuse(true);
  tbHttp.setTimeout(8000);
  tbHttp.addHeader("Content-Type", "application/json", false, true);
  unsigned long t0 = micros();
  int code = tbHttp.POST((uint8_t*)jsonBuf, strlen(jsonBuf));
  unsigned long rttUs = micros() - t0;
  bool ok = (code >= 200 && code < 300);
  Serial.printf("CLOUD_LOG,%s,%s,HTTP,%lu,%llu,%d,%u,%lu\n",
                EXPERIMENT_RUN_ID, NODE_ID,
                (unsigned long)seq, (unsigned long long)txMs,
                code, (unsigned)payloadBytes, rttUs);
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

  // setup pin sensor (hanya yang pakai sensor asli)
  #if USE_FAKE_SENSOR_SLOT1 == 0
    pinMode(IR_SLOT1, INPUT);
  #endif
  #if USE_FAKE_SENSOR_SLOT2 == 0
    pinMode(IR_SLOT2, INPUT);
  #endif
  #if USE_FAKE_SENSOR_SLOT3 == 0
    pinMode(IR_SLOT3, INPUT);
  #endif

  loadIPFromEEPROM(mqtt_local);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.addParameter(&custom_mqtt_local);
  wm.setConfigPortalTimeout(180);

  Serial.printf("Starting WiFiManager for Node %s...\n", NODE_ID);
  if (!wm.autoConnect(AP_PORTAL_NAME, WIFI_AP_PASSWORD)) {
    Serial.println("WiFi failed; restarting.");
    delay(2000);
    ESP.restart();
  }

  const char* enteredIp = custom_mqtt_local.getValue();
  if (enteredIp && strlen(enteredIp) > 0)
    strlcpy(mqtt_local, enteredIp, sizeof(mqtt_local));

  Serial.printf("WiFi connected. IP=%s, Mosquitto=%s\n",
                WiFi.localIP().toString().c_str(), mqtt_local);

  clockReady = syncClock();

  tbSecureClient.setInsecure();
  tbClient.setServer(TB_HOST, TB_PORT_MQTT);
  tbClient.setKeepAlive(30);
  tbClient.setSocketTimeout(8);
  tbClient.setBufferSize(512);

  localMqtt.setServer(mqtt_local, LOCAL_MQTT_PORT);
  localMqtt.setBufferSize(512);
  localMqtt.setKeepAlive(30);

  reconnectLocal();
#if USE_HTTP == 0
  reconnectTB();
#endif

  Serial.println("LOG_HEADER,run_id,node,protocol,seq,tx_epoch_ms,result,payload_bytes,duration_us");
  Serial.printf("READY,Node %s,protocol=%s,run=%s,clock=%s\n",
                NODE_ID, USE_HTTP ? "HTTP" : "MQTT",
                EXPERIMENT_RUN_ID, clockReady ? "OK" : "NOT_READY");

  kirimTelegram(String("Node ") + NODE_ID + " online");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) { delay(10); return; }

  reconnectLocal();
  if (localMqtt.connected()) localMqtt.loop();
#if USE_HTTP == 0
  reconnectTB();
  if (tbClient.connected()) tbClient.loop();
#endif

  unsigned long now = millis();
  if (now - lastPublish < PUBLISH_INTERVAL) return;
  lastPublish = now;

  // baca sensor (dengan fake switch per slot)
  String slot1 = bacaSensor(IR_SLOT1, USE_FAKE_SENSOR_SLOT1);
  String slot2 = bacaSensor(IR_SLOT2, USE_FAKE_SENSOR_SLOT2);
  String slot3 = bacaSensor(IR_SLOT3, USE_FAKE_SENSOR_SLOT3);

  int slotKosong = (slot1 == "KOSONG") + (slot2 == "KOSONG") + (slot3 == "KOSONG");
  String blok    = hitungStatusBlok(slot1, slot2, slot3);

  // 1. publish ke broker lokal (untuk sistem pusat)
  // kirim lokal dulu sebelum operasi cloud yang lambat
  {
    StaticJsonDocument<320> localDoc;
    localDoc["node_id"]    = NODE_ID;
    localDoc["slot1"]      = slot1;
    localDoc["slot2"]      = slot2;
    localDoc["slot3"]      = slot3;
    localDoc["slot_kosong"] = slotKosong;
    localDoc["blok"]       = blok;
    localDoc["local_seq"]  = localSeq++;
    char localBuf[320];
    size_t localLen = serializeJson(localDoc, localBuf, sizeof(localBuf));
    if (localLen > 0) {
      String topic = String(LOCAL_TOPIC_PREFIX) + NODE_ID + "/status";
      publishLokal(topic.c_str(), localBuf);
    }
  }

  // 2. publish ke ThingsBoard (dengan warmup dan limit)
  bool cloudLimitReached = (MAX_CLOUD_MESSAGES > 0 && cloudAttempts >= MAX_CLOUD_MESSAGES);
  bool isWarmup          = (warmupAttempts < WARMUP_CLOUD_MESSAGES);

  if ((!cloudLimitReached || isWarmup) && clockReady) {
#if USE_HTTP == 0
    if (!tbClient.connected()) {
      Serial.println("[TB-MQTT] disconnected before timestamp; interval skipped.");
      goto after_cloud; // lewati cloud publish interval ini
    }
#endif
    {
      uint32_t seq  = cloudSeq++;
      uint64_t txMs = epochMillis();

      if (txMs > 0) {
        StaticJsonDocument<384> cloudDoc;
        cloudDoc["node_id"]       = NODE_ID;
        cloudDoc["slot1"]         = slot1;
        cloudDoc["slot2"]         = slot2;
        cloudDoc["slot3"]         = slot3;
        cloudDoc["slot_kosong"]   = slotKosong;
        cloudDoc["blok"]          = blok;
        cloudDoc["run_id"]        = EXPERIMENT_RUN_ID;
        cloudDoc["seq"]           = seq;
        cloudDoc["send_timestamp"] = (unsigned long long)txMs;
        cloudDoc["protokol"]      = USE_HTTP ? "HTTP" : "MQTT";
        cloudDoc["phase"]         = isWarmup ? "WARMUP" : "MEASURE";
        // tambahan: status fake sensor untuk transparansi data
        cloudDoc["fake_s1"]       = USE_FAKE_SENSOR_SLOT1;
        cloudDoc["fake_s2"]       = USE_FAKE_SENSOR_SLOT2;
        cloudDoc["fake_s3"]       = USE_FAKE_SENSOR_SLOT3;

        char cloudBuf[384];
        size_t cloudLen = serializeJson(cloudDoc, cloudBuf, sizeof(cloudBuf));
        if (cloudLen > 0) {
          if (isWarmup) warmupAttempts++;
          else cloudAttempts++;
          sendToThingsBoard(cloudBuf, seq, txMs);
        }
      } else {
        Serial.println("[TIME] Invalid epoch; cloud sample skipped.");
      }
    }
  }

  after_cloud:

  // 3. debug serial
  Serial.printf("PARKING,%s,%s/%s/%s,kosong=%d,blok=%s\n",
                NODE_ID, slot1.c_str(), slot2.c_str(), slot3.c_str(),
                slotKosong, blok.c_str());

  // 4. notif Telegram jika status blok berubah
  if (blok != lastStatusBlok && lastStatusBlok.length() > 0) {
    String msg = String("Node ") + NODE_ID +
                 " - Blok " + blok +
                 " - slot kosong " + String(slotKosong) + "/3";
    kirimTelegram(msg);
  }
  lastStatusBlok = blok;
}

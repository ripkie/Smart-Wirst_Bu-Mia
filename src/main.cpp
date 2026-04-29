#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <time.h>
#include <ctype.h>
#include <stdlib.h>

#define RXD2 16
#define TXD2 17

const char *FIRESTORE_PROJECT_ID = "smart-wirst";

const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

char line[160];
int idx = 0;

bool wifiConnected = false;
bool timeReady = false;
bool expectResult = false;

unsigned long lastSendMs = 0;
int lastSbp = -1;
int lastDbp = -1;
int lastBpm = -1;

String activePatientId = "";
String activePatientName = "";
String activeNurseName = "";

int rotStep = 0;

int miringSbp = 0;
int miringDbp = 0;
int miringBpm = 0;

int terlentangSbp = 0;
int terlentangDbp = 0;
int terlentangBpm = 0;

void tampilkanHalamanWiFiBerhasil(unsigned long durasiMs = 10000)
{
  WebServer server(80);

  server.on("/", [&server]()
            {
    String html = "";
    html += "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>WiFi Tersambung</title>";
    html += "<style>";
    html += "body{font-family:Arial;text-align:center;padding:30px;background:#f4f7fb;}";
    html += ".card{background:white;border-radius:16px;padding:25px;max-width:420px;margin:auto;box-shadow:0 4px 20px rgba(0,0,0,.12);}";
    html += "h1{color:#188038;}p{font-size:18px;color:#333;}";
    html += ".ip{font-weight:bold;color:#1a73e8;}";
    html += "</style></head><body>";
    html += "<div class='card'>";
    html += "<h1>WiFi tersambung</h1>";
    html += "<p>Silahkan lakukan pengukuran tekanan darah.</p>";
    html += "<p>IP ESP32: <span class='ip'>" + WiFi.localIP().toString() + "</span></p>";
    html += "</div></body></html>";

    server.send(200, "text/html", html); });

  server.begin();

  Serial.println("[WIFI] Halaman sukses aktif sementara.");
  Serial.print("[WIFI] Buka browser: http://");
  Serial.println(WiFi.localIP());

  unsigned long startMs = millis();
  while (millis() - startMs < durasiMs)
  {
    server.handleClient();
    delay(10);
  }

  server.stop();
  Serial.println("[WIFI] Halaman sukses ditutup.");
}

void initWiFiConfig()
{
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setConnectTimeout(10);
  wm.setConfigPortalTimeout(180);

  Serial.println("[WIFI] Init WiFi config...");
  Serial.println("[WIFI] Jika belum tersimpan, connect HP ke hotspot: ESP32-BP-Setup");

  bool connected = wm.autoConnect("mamacare - wifi setup");

  if (connected)
  {
    wifiConnected = true;

    Serial.println("[WIFI] WiFi tersambung, silahkan lakukan pengukuran.");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());

    tampilkanHalamanWiFiBerhasil(10000);
  }
  else
  {
    Serial.println("[WIFI] Gagal connect / config timeout");
  }

  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  timeReady = false;

  Serial.println("[WIFI] Dimatikan setelah init");
}

String firestoreBaseUrl()
{
  return String("https://firestore.googleapis.com/v1/projects/") +
         FIRESTORE_PROJECT_ID +
         "/databases/(default)/documents";
}

void connectWiFiIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return;
  }

  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  // Coba connect ke WiFi tersimpan maksimal 10 detik.
  // Kalau gagal, ESP32 akan membuka hotspot konfigurasi.
  wm.setConnectTimeout(10);

  // Portal konfigurasi akan mati otomatis jika tidak ada input selama 180 detik.
  // Setelah itu fungsi return false dan pengiriman data dibatalkan.
  wm.setConfigPortalTimeout(180);

  Serial.println("[WIFI] Coba connect WiFi tersimpan...");
  Serial.println("[WIFI] Jika gagal, connect HP ke hotspot: ESP32-BP-Setup");

  bool connected = wm.autoConnect("Mamacare - Wifi Setup");

  if (!connected)
  {
    Serial.println("[WIFI] Gagal connect / config timeout");
    wifiConnected = false;
    return;
  }

  wifiConnected = true;

  Serial.print("[WIFI] Connected. IP: ");
  Serial.println(WiFi.localIP());

  delay(1000);
}

void disconnectWiFi()
{
  // Jangan pakai WiFi.disconnect(true, true), karena itu menghapus WiFi tersimpan.
  // Dengan ini, WiFiManager tetap bisa auto-connect ke WiFi terakhir.
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  timeReady = false;
  Serial.println("[WIFI] Dimatikan");
  delay(500);
}

void setupTimeIfNeeded()
{
  if (!wifiConnected)
    return;

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  int retry = 0;

  Serial.print("[TIME] Sinkronisasi waktu");

  while (!getLocalTime(&timeinfo) && retry < 20)
  {
    delay(500);
    Serial.print(".");
    retry++;
  }

  Serial.println();

  timeReady = getLocalTime(&timeinfo);

  Serial.println(timeReady ? "[TIME] OK" : "[TIME] Gagal");
}

bool getDateTimeString(char *out, size_t outSize, unsigned long &epochMs)
{
  struct tm timeinfo;
  time_t now;

  if (!timeReady || !getLocalTime(&timeinfo))
  {
    epochMs = 0;
    snprintf(out, outSize, "1970-01-01 00:00:00");
    return false;
  }

  time(&now);
  epochMs = (unsigned long)now * 1000UL;
  strftime(out, outSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
  return true;
}

String getStringField(String json, String fieldName)
{
  String key = "\"" + fieldName + "\"";
  int keyIndex = json.indexOf(key);
  if (keyIndex < 0)
    return "";

  int stringValueIndex = json.indexOf("\"stringValue\"", keyIndex);
  if (stringValueIndex < 0)
    return "";

  int colonIndex = json.indexOf(":", stringValueIndex);
  int firstQuote = json.indexOf("\"", colonIndex + 1);
  int secondQuote = json.indexOf("\"", firstQuote + 1);

  if (firstQuote < 0 || secondQuote < 0)
    return "";

  return json.substring(firstQuote + 1, secondQuote);
}

bool bacaActiveSessionFirestore()
{
  String url = firestoreBaseUrl() + "/settings/activeSession";

  for (int attempt = 1; attempt <= 3; attempt++)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      wifiConnected = false;
      connectWiFiIfNeeded();
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient https;
    https.setTimeout(20000);

    Serial.print("[FIRESTORE] GET activeSession attempt ");
    Serial.println(attempt);

    if (!https.begin(client, url))
    {
      https.end();
      delay(1000);
      continue;
    }

    int httpCode = https.GET();

    Serial.print("[FIRESTORE] GET code: ");
    Serial.println(httpCode);

    if (httpCode >= 200 && httpCode < 300)
    {
      String response = https.getString();
      https.end();

      activePatientId = getStringField(response, "patientId");
      activePatientName = getStringField(response, "patientName");
      activeNurseName = getStringField(response, "nurseName");

      Serial.print("Patient ID   : ");
      Serial.println(activePatientId);
      Serial.print("Patient Name : ");
      Serial.println(activePatientName);
      Serial.print("Nurse Name   : ");
      Serial.println(activeNurseName);

      return activePatientId.length() > 0;
    }

    if (httpCode > 0)
    {
      Serial.println(https.getString());
    }

    https.end();
    delay(1500);
  }

  return false;
}

bool parseHexRecord(const char *str, uint8_t *out, int &count)
{
  count = 0;

  while (*str)
  {
    while (*str == ' ')
      str++;

    if (!isxdigit((unsigned char)str[0]) || !isxdigit((unsigned char)str[1]))
    {
      return count > 0;
    }

    char hexByte[3];
    hexByte[0] = str[0];
    hexByte[1] = str[1];
    hexByte[2] = '\0';

    out[count++] = (uint8_t)strtol(hexByte, nullptr, 16);
    str += 2;

    while (*str == ' ')
      str++;

    if (count >= 32)
      break;
  }

  return count > 0;
}

float hitungMAP(int sbp, int dbp)
{
  return dbp + ((sbp - dbp) / 3.0f);
}

bool httpPost(String url, String payload)
{
  for (int attempt = 1; attempt <= 3; attempt++)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      wifiConnected = false;
      connectWiFiIfNeeded();
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient https;
    https.setTimeout(20000);

    if (!https.begin(client, url))
    {
      https.end();
      delay(1000);
      continue;
    }

    https.addHeader("Content-Type", "application/json");

    int httpCode = https.POST(payload);

    Serial.print("[HTTP POST] code: ");
    Serial.println(httpCode);

    if (httpCode > 0)
    {
      Serial.println(https.getString());
    }

    https.end();

    if (httpCode >= 200 && httpCode < 300)
    {
      return true;
    }

    delay(1500);
  }

  return false;
}

bool httpPatch(String url, String payload)
{
  for (int attempt = 1; attempt <= 3; attempt++)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      wifiConnected = false;
      connectWiFiIfNeeded();
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient https;
    https.setTimeout(20000);

    if (!https.begin(client, url))
    {
      https.end();
      delay(1000);
      continue;
    }

    https.addHeader("Content-Type", "application/json");

    int httpCode = https.PATCH(payload);

    Serial.print("[HTTP PATCH] code: ");
    Serial.println(httpCode);

    if (httpCode > 0)
    {
      Serial.println(https.getString());
    }

    https.end();

    if (httpCode >= 200 && httpCode < 300)
    {
      return true;
    }

    delay(1500);
  }

  return false;
}

bool kirimSingleFirestore(int sbp, int dbp, int bpm, bool keepWifiOn = false)
{
  connectWiFiIfNeeded();
  if (!wifiConnected)
    return false;

  setupTimeIfNeeded();

  if (!bacaActiveSessionFirestore())
  {
    disconnectWiFi();
    return false;
  }

  char datetimeStr[32];
  unsigned long epochMs = 0;
  getDateTimeString(datetimeStr, sizeof(datetimeStr), epochMs);

  float mapValue = hitungMAP(sbp, dbp);

  String payload = "{";
  payload += "\"fields\":{";
  payload += "\"mode\":{\"stringValue\":\"single\"},";
  payload += "\"patientId\":{\"stringValue\":\"" + activePatientId + "\"},";
  payload += "\"patientName\":{\"stringValue\":\"" + activePatientName + "\"},";
  payload += "\"nurseName\":{\"stringValue\":\"" + activeNurseName + "\"},";
  payload += "\"sbp\":{\"integerValue\":\"" + String(sbp) + "\"},";
  payload += "\"dbp\":{\"integerValue\":\"" + String(dbp) + "\"},";
  payload += "\"bpm\":{\"integerValue\":\"" + String(bpm) + "\"},";
  payload += "\"map\":{\"doubleValue\":" + String(mapValue, 1) + "},";
  payload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"},";
  payload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"}";
  payload += "}}";

  String measurementUrl = firestoreBaseUrl() +
                          "/patients/" + activePatientId +
                          "/measurements";

  bool okMeasurement = httpPost(measurementUrl, payload);

  String latestPayload = "{";
  latestPayload += "\"fields\":{";
  latestPayload += "\"latestMeasurement\":{";
  latestPayload += "\"mapValue\":{";
  latestPayload += "\"fields\":{";
  latestPayload += "\"mode\":{\"stringValue\":\"single\"},";
  latestPayload += "\"patientId\":{\"stringValue\":\"" + activePatientId + "\"},";
  latestPayload += "\"patientName\":{\"stringValue\":\"" + activePatientName + "\"},";
  latestPayload += "\"nurseName\":{\"stringValue\":\"" + activeNurseName + "\"},";
  latestPayload += "\"sbp\":{\"integerValue\":\"" + String(sbp) + "\"},";
  latestPayload += "\"dbp\":{\"integerValue\":\"" + String(dbp) + "\"},";
  latestPayload += "\"bpm\":{\"integerValue\":\"" + String(bpm) + "\"},";
  latestPayload += "\"map\":{\"doubleValue\":" + String(mapValue, 1) + "},";
  latestPayload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"},";
  latestPayload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"}";
  latestPayload += "}}}}}";

  String latestUrl = firestoreBaseUrl() +
                     "/patients/" + activePatientId +
                     "?updateMask.fieldPaths=latestMeasurement";

  bool okLatest = httpPatch(latestUrl, latestPayload);

  bool success = okMeasurement && okLatest;

  if (!keepWifiOn)
  {
    disconnectWiFi();
  }

  return success;
}

bool kirimROTFirestore(int rot, bool keepWifiOn = false)
{
  connectWiFiIfNeeded();
  if (!wifiConnected)
    return false;

  setupTimeIfNeeded();

  if (!bacaActiveSessionFirestore())
  {
    disconnectWiFi();
    return false;
  }

  char datetimeStr[32];
  unsigned long epochMs = 0;
  getDateTimeString(datetimeStr, sizeof(datetimeStr), epochMs);

  String payload = "{";
  payload += "\"fields\":{";
  payload += "\"mode\":{\"stringValue\":\"rot\"},";
  payload += "\"patientId\":{\"stringValue\":\"" + activePatientId + "\"},";
  payload += "\"patientName\":{\"stringValue\":\"" + activePatientName + "\"},";
  payload += "\"nurseName\":{\"stringValue\":\"" + activeNurseName + "\"},";
  payload += "\"rot\":{\"integerValue\":\"" + String(rot) + "\"},";
  payload += "\"miring_dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  payload += "\"terlentang_dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  payload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"},";
  payload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"},";
  payload += "\"miring_kiri\":{\"mapValue\":{\"fields\":{";
  payload += "\"sbp\":{\"integerValue\":\"" + String(miringSbp) + "\"},";
  payload += "\"dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  payload += "\"bpm\":{\"integerValue\":\"" + String(miringBpm) + "\"}";
  payload += "}}},";
  payload += "\"terlentang\":{\"mapValue\":{\"fields\":{";
  payload += "\"sbp\":{\"integerValue\":\"" + String(terlentangSbp) + "\"},";
  payload += "\"dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  payload += "\"bpm\":{\"integerValue\":\"" + String(terlentangBpm) + "\"}";
  payload += "}}}";
  payload += "}}";

  String rotUrl = firestoreBaseUrl() +
                  "/patients/" + activePatientId +
                  "/rotLogs";

  bool okRotLog = httpPost(rotUrl, payload);

  String latestPayload = "{";
  latestPayload += "\"fields\":{";
  latestPayload += "\"latestROT\":{";
  latestPayload += "\"mapValue\":{";
  latestPayload += "\"fields\":{";
  latestPayload += "\"rot\":{\"integerValue\":\"" + String(rot) + "\"},";
  latestPayload += "\"miring_dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  latestPayload += "\"terlentang_dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  latestPayload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"},";
  latestPayload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"}";
  latestPayload += "}}}}}";

  String latestUrl = firestoreBaseUrl() +
                     "/patients/" + activePatientId +
                     "?updateMask.fieldPaths=latestROT";

  bool okLatest = httpPatch(latestUrl, latestPayload);

  bool success = okRotLog && okLatest;

  if (!keepWifiOn)
  {
    disconnectWiFi();
  }

  return success;
}

void prosesROT(int sbp, int dbp, int bpm)
{
  if (rotStep == 0)
  {
    miringSbp = sbp;
    miringDbp = dbp;
    miringBpm = bpm;

    bool ok = kirimSingleFirestore(sbp, dbp, bpm, false);

    Serial.println();
    Serial.println("================================");
    Serial.println("[ROT] Sampel 1/2");
    Serial.println("Posisi: MIRING KIRI");
    Serial.print("TD: ");
    Serial.print(sbp);
    Serial.print("/");
    Serial.println(dbp);
    Serial.print("BPM: ");
    Serial.println(bpm);
    Serial.println(ok ? "[FIRESTORE] Sampel 1 terkirim" : "[FIRESTORE] Sampel 1 gagal");
    Serial.println("Lanjut ukur posisi TERLENTANG");
    Serial.println("================================");

    rotStep = 1;
    return;
  }

  if (rotStep == 1)
  {
    terlentangSbp = sbp;
    terlentangDbp = dbp;
    terlentangBpm = bpm;

    int rot = terlentangDbp - miringDbp;

    connectWiFiIfNeeded();
    setupTimeIfNeeded();

    bool okSingle = kirimSingleFirestore(terlentangSbp, terlentangDbp, terlentangBpm, true);
    bool okRot = kirimROTFirestore(rot, true);

    disconnectWiFi();

    Serial.println();
    Serial.println("================================");
    Serial.println("[ROT] Sampel 2/2");
    Serial.println("Posisi: TERLENTANG");
    Serial.print("TD: ");
    Serial.print(terlentangSbp);
    Serial.print("/");
    Serial.println(terlentangDbp);
    Serial.print("BPM: ");
    Serial.println(terlentangBpm);
    Serial.print("ROT = ");
    Serial.print(terlentangDbp);
    Serial.print(" - ");
    Serial.print(miringDbp);
    Serial.print(" = ");
    Serial.println(rot);
    Serial.println(okSingle ? "[FIRESTORE] Sampel 2 terkirim" : "[FIRESTORE] Sampel 2 gagal");
    Serial.println(okRot ? "[FIRESTORE] ROT terkirim" : "[FIRESTORE] ROT gagal");
    Serial.println("================================");

    rotStep = 0;
  }
}

void prosesLine(char *s)
{
  if (strlen(s) == 0)
    return;

  if (strstr(s, "test return:0save record") != NULL)
  {
    expectResult = true;
    Serial.println("[INFO] Hasil final siap, menunggu record...");
    return;
  }

  if (!expectResult)
    return;

  uint8_t data[32];
  int len = 0;

  if (parseHexRecord(s, data, len) && len >= 4)
  {
    int sbp = data[0];
    int dbp = data[1];
    int bpm = data[3];

    if (sbp >= 60 && sbp <= 250 &&
        dbp >= 40 && dbp <= 180 &&
        bpm >= 30 && bpm <= 220)
    {

      expectResult = false;

      if (sbp == lastSbp &&
          dbp == lastDbp &&
          bpm == lastBpm &&
          millis() - lastSendMs < 10000)
      {
        Serial.println("[INFO] Data duplikat diabaikan");
        return;
      }

      prosesROT(sbp, dbp, bpm);

      lastSbp = sbp;
      lastDbp = dbp;
      lastBpm = bpm;
      lastSendMs = millis();
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  delay(1000);

  initWiFiConfig();

  Serial.println("=== BP Reader + Firestore + Auto ROT ===");
  Serial.println("WiFi OFF saat pengukuran.");
  Serial.println("Pengukuran 1 = kirim measurement + simpan miring.");
  Serial.println("Pengukuran 2 = kirim measurement + hitung/kirim ROT.");
}

void loop()
{
  while (Serial2.available())
  {
    char c = Serial2.read();

    if (c == '\r')
      continue;

    if (c == '\n')
    {
      line[idx] = '\0';
      prosesLine(line);
      idx = 0;
    }
    else
    {
      if (idx < (int)sizeof(line) - 1)
      {
        line[idx++] = c;
      }
    }
  }
}
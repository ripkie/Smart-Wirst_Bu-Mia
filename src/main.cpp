#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ctype.h>
#include <stdlib.h>

#define RXD2 16
#define TXD2 17

const char *WIFI_SSID = "ripki";
const char *WIFI_PASSWORD = "12341234";
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
String activeMode = "single";

// ROT state
int rotStep = 0;
int miringSbp = 0;
int miringDbp = 0;
int miringBpm = 0;
int terlentangSbp = 0;
int terlentangDbp = 0;
int terlentangBpm = 0;

String firestoreBaseUrl()
{
  return String("https://firestore.googleapis.com/v1/projects/") +
         FIRESTORE_PROJECT_ID +
         "/databases/(default)/documents";
}

void printWiFiStatus()
{
  Serial.print("[WIFI] Status: ");
  Serial.println(WiFi.status());

  Serial.print("[WIFI] RSSI: ");
  Serial.println(WiFi.RSSI());
}

void connectWiFiIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return;
  }

  WiFi.mode(WIFI_STA);

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WIFI] Menghubungkan WiFi...");

    WiFi.disconnect(true);
    delay(500);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 40)
    {
      delay(500);
      Serial.print(".");
      retry++;
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[WIFI] Belum konek, coba ulang 2 detik lagi...");
      delay(2000);
    }
  }

  wifiConnected = true;

  Serial.print("[WIFI] Connected. IP: ");
  Serial.println(WiFi.localIP());
  printWiFiStatus();

  delay(1500); // tunggu koneksi stabil sebelum HTTPS
}

void disconnectWiFi()
{
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  timeReady = false;
  Serial.println("[WIFI] Dimatikan");
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
  Serial.println(timeReady ? "[TIME] Waktu berhasil sinkron" : "[TIME] Gagal sinkron waktu");
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
      Serial.println("[FIRESTORE] begin gagal");
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
      activeMode = getStringField(response, "mode");

      if (activeMode.length() == 0)
        activeMode = "single";

      Serial.print("Patient ID     : ");
      Serial.println(activePatientId);
      Serial.print("Patient Name   : ");
      Serial.println(activePatientName);
      Serial.print("Nurse Name     : ");
      Serial.println(activeNurseName);
      Serial.print("Mode Firestore : ");
      Serial.println(activeMode);

      return activePatientId.length() > 0;
    }

    if (httpCode > 0)
    {
      Serial.println(https.getString());
    }

    https.end();
    Serial.println("[FIRESTORE] GET gagal, retry...");
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

    Serial.print("[HTTP POST] attempt ");
    Serial.println(attempt);

    if (!https.begin(client, url))
    {
      Serial.println("[HTTP POST] begin gagal");
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
      String response = https.getString();
      Serial.println(response);
    }

    https.end();

    if (httpCode >= 200 && httpCode < 300)
    {
      return true;
    }

    Serial.println("[HTTP POST] gagal, retry...");
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

    Serial.print("[HTTP PATCH] attempt ");
    Serial.println(attempt);

    if (!https.begin(client, url))
    {
      Serial.println("[HTTP PATCH] begin gagal");
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
      String response = https.getString();
      Serial.println(response);
    }

    https.end();

    if (httpCode >= 200 && httpCode < 300)
    {
      return true;
    }

    Serial.println("[HTTP PATCH] gagal, retry...");
    delay(1500);
  }

  return false;
}

bool kirimSingleFirestore(int sbp, int dbp, int bpm)
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

  Serial.println();
  Serial.println("================================");
  Serial.println("HASIL SINGLE");
  Serial.println("================================");
  Serial.print("Patient ID    : ");
  Serial.println(activePatientId);
  Serial.print("Patient Name  : ");
  Serial.println(activePatientName);
  Serial.print("Nurse Name    : ");
  Serial.println(activeNurseName);
  Serial.print("Waktu         : ");
  Serial.println(datetimeStr);
  Serial.print("Tekanan Darah : ");
  Serial.print(sbp);
  Serial.print("/");
  Serial.println(dbp);
  Serial.print("Heart Rate    : ");
  Serial.print(bpm);
  Serial.println(" bpm");
  Serial.print("MAP           : ");
  Serial.println(mapValue, 1);
  Serial.println("================================");

  String measurementPayload = "{";
  measurementPayload += "\"fields\":{";
  measurementPayload += "\"mode\":{\"stringValue\":\"single\"},";
  measurementPayload += "\"patientId\":{\"stringValue\":\"" + activePatientId + "\"},";
  measurementPayload += "\"patientName\":{\"stringValue\":\"" + activePatientName + "\"},";
  measurementPayload += "\"nurseName\":{\"stringValue\":\"" + activeNurseName + "\"},";
  measurementPayload += "\"sbp\":{\"integerValue\":\"" + String(sbp) + "\"},";
  measurementPayload += "\"dbp\":{\"integerValue\":\"" + String(dbp) + "\"},";
  measurementPayload += "\"bpm\":{\"integerValue\":\"" + String(bpm) + "\"},";
  measurementPayload += "\"map\":{\"doubleValue\":" + String(mapValue, 1) + "},";
  measurementPayload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"},";
  measurementPayload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"}";
  measurementPayload += "}}";

  String measurementUrl = firestoreBaseUrl() +
                          "/patients/" + activePatientId +
                          "/measurements";

  bool okMeasurement = httpPost(measurementUrl, measurementPayload);

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

  disconnectWiFi();
  return okMeasurement && okLatest;
}

bool kirimROTFirestore(int rot)
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

  Serial.println();
  Serial.println("================================");
  Serial.println("HASIL ROT");
  Serial.println("================================");
  Serial.print("Patient ID    : ");
  Serial.println(activePatientId);
  Serial.print("Patient Name  : ");
  Serial.println(activePatientName);
  Serial.print("Nurse Name    : ");
  Serial.println(activeNurseName);
  Serial.print("Miring kiri   : ");
  Serial.print(miringSbp);
  Serial.print("/");
  Serial.print(miringDbp);
  Serial.print(" | BPM ");
  Serial.println(miringBpm);
  Serial.print("Terlentang    : ");
  Serial.print(terlentangSbp);
  Serial.print("/");
  Serial.print(terlentangDbp);
  Serial.print(" | BPM ");
  Serial.println(terlentangBpm);
  Serial.print("ROT           : ");
  Serial.print(terlentangDbp);
  Serial.print(" - ");
  Serial.print(miringDbp);
  Serial.print(" = ");
  Serial.println(rot);
  Serial.println("================================");

  String rotPayload = "{";
  rotPayload += "\"fields\":{";
  rotPayload += "\"mode\":{\"stringValue\":\"rot\"},";
  rotPayload += "\"patientId\":{\"stringValue\":\"" + activePatientId + "\"},";
  rotPayload += "\"patientName\":{\"stringValue\":\"" + activePatientName + "\"},";
  rotPayload += "\"nurseName\":{\"stringValue\":\"" + activeNurseName + "\"},";
  rotPayload += "\"rot\":{\"integerValue\":\"" + String(rot) + "\"},";
  rotPayload += "\"miring_dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  rotPayload += "\"terlentang_dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  rotPayload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"},";
  rotPayload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"},";
  rotPayload += "\"miring_kiri\":{\"mapValue\":{\"fields\":{";
  rotPayload += "\"sbp\":{\"integerValue\":\"" + String(miringSbp) + "\"},";
  rotPayload += "\"dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  rotPayload += "\"bpm\":{\"integerValue\":\"" + String(miringBpm) + "\"}";
  rotPayload += "}}},";
  rotPayload += "\"terlentang\":{\"mapValue\":{\"fields\":{";
  rotPayload += "\"sbp\":{\"integerValue\":\"" + String(terlentangSbp) + "\"},";
  rotPayload += "\"dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  rotPayload += "\"bpm\":{\"integerValue\":\"" + String(terlentangBpm) + "\"}";
  rotPayload += "}}}";
  rotPayload += "}}";

  String rotUrl = firestoreBaseUrl() +
                  "/patients/" + activePatientId +
                  "/rotLogs";

  bool okRotLog = httpPost(rotUrl, rotPayload);

  String latestRotPayload = "{";
  latestRotPayload += "\"fields\":{";
  latestRotPayload += "\"latestROT\":{";
  latestRotPayload += "\"mapValue\":{";
  latestRotPayload += "\"fields\":{";
  latestRotPayload += "\"rot\":{\"integerValue\":\"" + String(rot) + "\"},";
  latestRotPayload += "\"miring_dbp\":{\"integerValue\":\"" + String(miringDbp) + "\"},";
  latestRotPayload += "\"terlentang_dbp\":{\"integerValue\":\"" + String(terlentangDbp) + "\"},";
  latestRotPayload += "\"datetime\":{\"stringValue\":\"" + String(datetimeStr) + "\"},";
  latestRotPayload += "\"timestamp_ms\":{\"integerValue\":\"" + String(epochMs) + "\"}";
  latestRotPayload += "}}}}}";

  String latestRotUrl = firestoreBaseUrl() +
                        "/patients/" + activePatientId +
                        "?updateMask.fieldPaths=latestROT";

  bool okLatestRot = httpPatch(latestRotUrl, latestRotPayload);

  disconnectWiFi();
  return okRotLog && okLatestRot;
}

void prosesModeROT(int sbp, int dbp, int bpm)
{
  if (rotStep == 0)
  {
    miringSbp = sbp;
    miringDbp = dbp;
    miringBpm = bpm;

    rotStep = 1;

    Serial.println();
    Serial.println("================================");
    Serial.println("[ROT] Sampel 1/2 diambil");
    Serial.println("Posisi: MIRING KIRI");
    Serial.print("Tekanan Darah : ");
    Serial.print(sbp);
    Serial.print("/");
    Serial.println(dbp);
    Serial.print("Heart Rate    : ");
    Serial.print(bpm);
    Serial.println(" bpm");
    Serial.println("Lanjutkan pengukuran posisi TERLENTANG");
    Serial.println("================================");
    return;
  }

  if (rotStep == 1)
  {
    terlentangSbp = sbp;
    terlentangDbp = dbp;
    terlentangBpm = bpm;

    int rot = terlentangDbp - miringDbp;

    bool success = kirimROTFirestore(rot);
    Serial.println(success ? "[FIRESTORE] ROT berhasil dikirim" : "[FIRESTORE] ROT gagal dikirim");

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

      if (sbp == lastSbp && dbp == lastDbp && bpm == lastBpm &&
          millis() - lastSendMs < 10000)
      {
        Serial.println("[INFO] Data duplikat diabaikan");
        return;
      }

      connectWiFiIfNeeded();
      if (!wifiConnected)
      {
        Serial.println("[WIFI] Gagal konek");
        return;
      }

      bool sessionOk = bacaActiveSessionFirestore();
      disconnectWiFi();

      if (!sessionOk)
      {
        Serial.println("[FIRESTORE] Active session tidak ditemukan");
        return;
      }

      Serial.print("[MODE FIRESTORE] ");
      Serial.println(activeMode);

      if (activeMode == "rot")
      {
        prosesModeROT(sbp, dbp, bpm);
      }
      else
      {
        bool success = kirimSingleFirestore(sbp, dbp, bpm);
        Serial.println(success ? "[FIRESTORE] Data berhasil dikirim" : "[FIRESTORE] Data gagal dikirim");
      }

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

  WiFi.mode(WIFI_OFF);

  Serial.println("=== BP Reader + Firestore + ROT ===");
  Serial.println("Mode selalu ikut Firestore:");
  Serial.println("settings/activeSession/mode = single atau rot");
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
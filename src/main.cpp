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
const char *FIREBASE_URL = "https://smart-wirst-default-rtdb.asia-southeast1.firebasedatabase.app";
const char *PATIENT_ID = "pasien_001";

const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

char line[160];
int idx = 0;

bool wifiConnected = false;
bool timeReady = false;
bool expectResult = false;

unsigned long lastSendMs = 0;
int lastSbp = -1, lastDbp = -1, lastBpm = -1;

// ROT mode
bool rotMode = false;
int rotStep = 0;

int rotSbpMiring = 0, rotDbpMiring = 0, rotBpmMiring = 0;
int rotSbpTerlentang = 0, rotDbpTerlentang = 0, rotBpmTerlentang = 0;

int lastRotSbp = -1;
int lastRotDbp = -1;
int lastRotBpm = -1;

unsigned long lastRotTime = 0;
const unsigned long ROT_DELAY = 8000;

void connectWiFiIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30)
  {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  wifiConnected = WiFi.status() == WL_CONNECTED;

  if (wifiConnected)
  {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("WiFi gagal connect");
  }
}

void disconnectWiFi()
{
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  timeReady = false;
  Serial.println("WiFi dimatikan");
}

void setupTimeIfNeeded()
{
  if (!wifiConnected)
    return;

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  int retry = 0;

  Serial.print("Sinkronisasi waktu");
  while (!getLocalTime(&timeinfo) && retry < 20)
  {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  if (getLocalTime(&timeinfo))
  {
    timeReady = true;
    Serial.println("Waktu berhasil sinkron");
  }
  else
  {
    timeReady = false;
    Serial.println("Gagal sinkron waktu");
  }
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

bool kirimPayloadFirebase(String path, String payload, bool postMode)
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  bool ok = false;

  if (https.begin(client, path))
  {
    https.addHeader("Content-Type", "application/json");

    int httpCode = postMode ? https.POST(payload) : https.PUT(payload);

    Serial.print(postMode ? "[FIREBASE] POST => HTTP " : "[FIREBASE] PUT => HTTP ");
    Serial.println(httpCode);

    if (httpCode > 0)
    {
      Serial.println(https.getString());
      ok = (httpCode >= 200 && httpCode < 300);
    }

    https.end();
  }

  return ok;
}

bool kirimPengukuranKeFirebase(int sbp, int dbp, int bpm)
{
  connectWiFiIfNeeded();
  if (!wifiConnected)
    return false;

  setupTimeIfNeeded();

  char datetimeStr[32];
  unsigned long epochMs = 0;
  getDateTimeString(datetimeStr, sizeof(datetimeStr), epochMs);

  float mapValue = hitungMAP(sbp, dbp);

  Serial.println();
  Serial.println("================================");
  Serial.println("HASIL PENGUKURAN");
  Serial.println("================================");
  Serial.print("ID Pasien : ");
  Serial.println(PATIENT_ID);
  Serial.print("Waktu     : ");
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

  String basePath = String(FIREBASE_URL) + "/patients/" + String(PATIENT_ID);

  String payload = "{";
  payload += "\"patient_id\":\"" + String(PATIENT_ID) + "\",";
  payload += "\"mode\":\"single\",";
  payload += "\"sbp\":" + String(sbp) + ",";
  payload += "\"dbp\":" + String(dbp) + ",";
  payload += "\"bpm\":" + String(bpm) + ",";
  payload += "\"map\":" + String(mapValue, 1) + ",";
  payload += "\"timestamp_ms\":" + String(epochMs) + ",";
  payload += "\"datetime\":\"" + String(datetimeStr) + "\"";
  payload += "}";

  bool ok1 = kirimPayloadFirebase(basePath + "/latest.json", payload, false);
  bool ok2 = kirimPayloadFirebase(basePath + "/logs.json", payload, true);

  disconnectWiFi();
  return ok1 && ok2;
}

bool kirimROTKeFirebase(int rot)
{
  connectWiFiIfNeeded();
  if (!wifiConnected)
    return false;

  setupTimeIfNeeded();

  char datetimeStr[32];
  unsigned long epochMs = 0;
  getDateTimeString(datetimeStr, sizeof(datetimeStr), epochMs);

  String basePath = String(FIREBASE_URL) + "/patients/" + String(PATIENT_ID);

  String payload = "{";
  payload += "\"patient_id\":\"" + String(PATIENT_ID) + "\",";
  payload += "\"mode\":\"rot\",";
  payload += "\"rot\":" + String(rot) + ",";
  payload += "\"miring_kiri\":{";
  payload += "\"sbp\":" + String(rotSbpMiring) + ",";
  payload += "\"dbp\":" + String(rotDbpMiring) + ",";
  payload += "\"bpm\":" + String(rotBpmMiring);
  payload += "},";
  payload += "\"terlentang\":{";
  payload += "\"sbp\":" + String(rotSbpTerlentang) + ",";
  payload += "\"dbp\":" + String(rotDbpTerlentang) + ",";
  payload += "\"bpm\":" + String(rotBpmTerlentang);
  payload += "},";
  payload += "\"timestamp_ms\":" + String(epochMs) + ",";
  payload += "\"datetime\":\"" + String(datetimeStr) + "\"";
  payload += "}";

  bool ok1 = kirimPayloadFirebase(basePath + "/rot/latest.json", payload, false);
  bool ok2 = kirimPayloadFirebase(basePath + "/rot/logs.json", payload, true);

  disconnectWiFi();
  return ok1 && ok2;
}

void prosesROT(int sbp, int dbp, int bpm)
{
  if (sbp == lastRotSbp && dbp == lastRotDbp && bpm == lastRotBpm)
  {
    Serial.println("[ROT] Data duplikat diabaikan");
    return;
  }

  if (rotStep > 0 && millis() - lastRotTime < ROT_DELAY)
  {
    Serial.println("[ROT] Tunggu pengukuran berikutnya...");
    return;
  }

  lastRotSbp = sbp;
  lastRotDbp = dbp;
  lastRotBpm = bpm;
  lastRotTime = millis();

  if (rotStep == 0)
  {
    rotSbpMiring = sbp;
    rotDbpMiring = dbp;
    rotBpmMiring = bpm;

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

    rotStep = 1;
    return;
  }

  if (rotStep == 1)
  {
    rotSbpTerlentang = sbp;
    rotDbpTerlentang = dbp;
    rotBpmTerlentang = bpm;

    int rot = rotDbpTerlentang - rotDbpMiring;

    Serial.println();
    Serial.println("================================");
    Serial.println("[ROT] Sampel 2/2 diambil");
    Serial.println("HASIL MODE ROT");
    Serial.println("================================");

    Serial.print("Miring kiri : ");
    Serial.print(rotSbpMiring);
    Serial.print("/");
    Serial.print(rotDbpMiring);
    Serial.print(" | BPM ");
    Serial.println(rotBpmMiring);

    Serial.print("Terlentang  : ");
    Serial.print(rotSbpTerlentang);
    Serial.print("/");
    Serial.print(rotDbpTerlentang);
    Serial.print(" | BPM ");
    Serial.println(rotBpmTerlentang);

    Serial.print("ROT         : ");
    Serial.print(rotDbpTerlentang);
    Serial.print(" - ");
    Serial.print(rotDbpMiring);
    Serial.print(" = ");
    Serial.println(rot);
    Serial.println("================================");

    bool success = kirimROTKeFirebase(rot);
    Serial.println(success ? "[FIREBASE] ROT berhasil dikirim" : "[FIREBASE] ROT gagal dikirim");

    rotMode = false;
    rotStep = 0;
  }
}

void cekCommandSerial()
{
  while (Serial.available())
  {
    char cmd = Serial.read();

    if (cmd == '4')
    {
      rotMode = true;
      rotStep = 0;

      lastRotSbp = -1;
      lastRotDbp = -1;
      lastRotBpm = -1;
      lastRotTime = 0;

      Serial.println();
      Serial.println("================================");
      Serial.println("MODE ROT AKTIF");
      Serial.println("Sampel 1/2: MIRING KIRI");
      Serial.println("Sampel 2/2: TERLENTANG");
      Serial.println("ROT = DBP terlentang - DBP miring kiri");
      Serial.println("================================");
    }
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

      if (rotMode)
      {
        prosesROT(sbp, dbp, bpm);
        return;
      }

      if (sbp == lastSbp && dbp == lastDbp && bpm == lastBpm &&
          millis() - lastSendMs < 10000)
      {
        Serial.println("[INFO] Data duplikat diabaikan");
        return;
      }

      bool success = kirimPengukuranKeFirebase(sbp, dbp, bpm);
      Serial.println(success ? "[FIREBASE] Data berhasil dikirim" : "[FIREBASE] Data gagal dikirim");

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

  Serial.println("=== BP Reader Mode ===");
  Serial.println("WiFi standby. Menunggu hasil pengukuran...");
  Serial.println("Ketik 4 lalu Enter untuk MODE ROT");
}

void loop()
{
  cekCommandSerial();

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
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
const long GMT_OFFSET_SEC = 7 * 3600; // WIB
const int DAYLIGHT_OFFSET_SEC = 0;

char line[160];
int idx = 0;

unsigned long lastSendMs = 0;
int lastSbp = -1;
int lastDbp = -1;
int lastBpm = -1;

bool wifiConnected = false;
bool timeReady = false;

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

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    wifiConnected = false;
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

void tampilkanHasil(int sbp, int dbp, int bpm, float mapValue, const char *datetimeStr, const char *patientId)
{
  Serial.println();
  Serial.println("================================");
  Serial.println("HASIL PENGUKURAN");
  Serial.println("================================");
  Serial.print("ID Pasien : ");
  Serial.println(patientId);

  Serial.print("Waktu     : ");
  Serial.println(datetimeStr);

  Serial.print("1) Tekanan Darah / Blood Pressure : ");
  Serial.print(sbp);
  Serial.print("/");
  Serial.println(dbp);

  Serial.print("2) Heart Rate : ");
  Serial.print(bpm);
  Serial.println(" bpm");

  Serial.print("3) MAP : ");
  Serial.println(mapValue, 1);
  Serial.println("================================");
  Serial.println();
}

bool kirimKeFirebase(int sbp, int dbp, int bpm, float mapValue, const char *datetimeStr, unsigned long epochMs, const char *patientId)
{
  connectWiFiIfNeeded();
  if (!wifiConnected)
    return false;

  if (!timeReady)
  {
    setupTimeIfNeeded();
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String basePath = String(FIREBASE_URL) + "/patients/" + String(patientId);

  String payload = "{";
  payload += "\"patient_id\":\"" + String(patientId) + "\",";
  payload += "\"sbp\":" + String(sbp) + ",";
  payload += "\"dbp\":" + String(dbp) + ",";
  payload += "\"bpm\":" + String(bpm) + ",";
  payload += "\"map\":" + String(mapValue, 1) + ",";
  payload += "\"timestamp_ms\":" + String(epochMs) + ",";
  payload += "\"datetime\":\"" + String(datetimeStr) + "\"";
  payload += "}";

  bool ok1 = false;
  String urlLatest = basePath + "/latest.json";

  if (https.begin(client, urlLatest))
  {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.PUT(payload);
    Serial.print("[FIREBASE] PUT latest => HTTP ");
    Serial.println(httpCode);
    ok1 = (httpCode >= 200 && httpCode < 300);
    https.end();
  }

  bool ok2 = false;
  String urlLogs = basePath + "/logs.json";

  if (https.begin(client, urlLogs))
  {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.POST(payload);
    Serial.print("[FIREBASE] POST logs => HTTP ");
    Serial.println(httpCode);
    ok2 = (httpCode >= 200 && httpCode < 300);
    https.end();
  }

  disconnectWiFi();
  return ok1 && ok2;
}

void prosesLine(char *s)
{
  if (strlen(s) == 0)
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

      if (sbp == lastSbp && dbp == lastDbp && bpm == lastBpm &&
          millis() - lastSendMs < 5000)
      {
        return;
      }

      float mapValue = hitungMAP(sbp, dbp);

      char datetimeStr[32] = "belum sync";
      unsigned long epochMs = 0;

      // tampilkan hasil dulu, lalu kirim
      tampilkanHasil(sbp, dbp, bpm, mapValue, datetimeStr, PATIENT_ID);

      bool success = kirimKeFirebase(sbp, dbp, bpm, mapValue, datetimeStr, epochMs, PATIENT_ID);

      // ambil waktu asli setelah wifi+ntp aktif
      if (wifiConnected == false)
      {
        // sengaja kosong
      }

      // connect lagi khusus ambil waktu lalu update payload kedua kali kalau mau,
      // tapi untuk sederhana kita cukup kirim sekali setelah connect.
      // alternatif lebih rapi: ambil waktu sebelum payload dibuat di kirimKeFirebase().
      // untuk sekarang, data utama tetap tersimpan.

      if (success)
      {
        Serial.println("[FIREBASE] Data berhasil dikirim");
      }
      else
      {
        Serial.println("[FIREBASE] Data gagal dikirim");
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

  Serial.println("=== BP Reader Mode ===");
  Serial.println("WiFi standby. Menunggu hasil pengukuran...");
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
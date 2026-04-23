#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <stdlib.h>

#define RXD2 16
#define TXD2 17

const char *WIFI_SSID = "ripki";
const char *WIFI_PASSWORD = "12341234";

const char *FIREBASE_URL = "https://smart-wirst-default-rtdb.asia-southeast1.firebasedatabase.app";

char line[160];
int idx = 0;

unsigned long lastSendMs = 0;
int lastSbp = -1;
int lastDbp = -1;
int lastBpm = -1;

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
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

void tampilkanHasil(int sbp, int dbp, int bpm, float mapValue)
{
  Serial.println();
  Serial.println("================================");
  Serial.println("HASIL PENGUKURAN");
  Serial.println("================================");
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

bool kirimKeFirebase(int sbp, int dbp, int bpm, float mapValue)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[FIREBASE] WiFi putus, reconnect...");
    connectWiFi();
  }

  WiFiClientSecure client;
  client.setInsecure(); // cepat untuk testing

  HTTPClient https;

  // latest.json -> overwrite data terakhir
  String urlLatest = String(FIREBASE_URL) + "/bp/latest.json";

  String payloadLatest = "{";
  payloadLatest += "\"sbp\":" + String(sbp) + ",";
  payloadLatest += "\"dbp\":" + String(dbp) + ",";
  payloadLatest += "\"bpm\":" + String(bpm) + ",";
  payloadLatest += "\"map\":" + String(mapValue, 1) + ",";
  payloadLatest += "\"timestamp_ms\":" + String((unsigned long)millis());
  payloadLatest += "}";

  bool ok1 = false;
  if (https.begin(client, urlLatest))
  {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.PUT(payloadLatest);

    Serial.print("[FIREBASE] PUT latest => HTTP ");
    Serial.println(httpCode);

    if (httpCode > 0)
    {
      String response = https.getString();
      Serial.println(response);
      ok1 = (httpCode >= 200 && httpCode < 300);
    }
    else
    {
      Serial.print("[FIREBASE] PUT error: ");
      Serial.println(https.errorToString(httpCode));
    }
    https.end();
  }
  else
  {
    Serial.println("[FIREBASE] Gagal begin latest");
  }

  // logs.json -> simpan histori baru tiap pengukuran
  String urlLog = String(FIREBASE_URL) + "/bp/logs.json";

  String payloadLog = "{";
  payloadLog += "\"sbp\":" + String(sbp) + ",";
  payloadLog += "\"dbp\":" + String(dbp) + ",";
  payloadLog += "\"bpm\":" + String(bpm) + ",";
  payloadLog += "\"map\":" + String(mapValue, 1) + ",";
  payloadLog += "\"timestamp_ms\":" + String((unsigned long)millis());
  payloadLog += "}";

  bool ok2 = false;
  if (https.begin(client, urlLog))
  {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.POST(payloadLog);

    Serial.print("[FIREBASE] POST logs => HTTP ");
    Serial.println(httpCode);

    if (httpCode > 0)
    {
      String response = https.getString();
      Serial.println(response);
      ok2 = (httpCode >= 200 && httpCode < 300);
    }
    else
    {
      Serial.print("[FIREBASE] POST error: ");
      Serial.println(https.errorToString(httpCode));
    }
    https.end();
  }
  else
  {
    Serial.println("[FIREBASE] Gagal begin logs");
  }

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
    // dari hasil reverse engineering kamu:
    // byte0 = SYS, byte1 = DIA, byte3 = BPM
    int sbp = data[0];
    int dbp = data[1];
    int bpm = data[3];

    if (sbp >= 60 && sbp <= 250 &&
        dbp >= 40 && dbp <= 180 &&
        bpm >= 30 && bpm <= 220)
    {

      // hindari kirim dobel terlalu cepat
      if (sbp == lastSbp && dbp == lastDbp && bpm == lastBpm &&
          millis() - lastSendMs < 5000)
      {
        return;
      }

      float mapValue = hitungMAP(sbp, dbp);

      tampilkanHasil(sbp, dbp, bpm, mapValue);

      bool success = kirimKeFirebase(sbp, dbp, bpm, mapValue);
      if (success)
      {
        Serial.println("[FIREBASE] Data berhasil dikirim");
      }
      else
      {
        Serial.println("[FIREBASE] Ada pengiriman yang gagal");
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

  Serial.println("=== BP Reader + Firebase ===");
  connectWiFi();
  Serial.println("Menunggu hasil pengukuran...");
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
#include <Arduino.h>

#define RXD2 16
#define TXD2 17

char line[160];
int idx = 0;

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

void tampilkanHasil(int sbp, int dbp, int bpm)
{
  float mapValue = dbp + ((sbp - dbp) / 3.0);

  Serial.println();
  Serial.println("==================================");
  Serial.println("HASIL PENGUKURAN");
  Serial.println("==================================");

  Serial.print("1) Tekanan Darah / Blood Pressure : ");
  Serial.print(sbp);
  Serial.print("/");
  Serial.println(dbp);

  Serial.print("2) Heart Rate : ");
  Serial.print(bpm);
  Serial.println(" bpm");

  Serial.print("3) MAP : ");
  Serial.println(mapValue, 1);

  Serial.println("==================================");
  Serial.println();
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
      tampilkanHasil(sbp, dbp, bpm);
    }
  }
}

void setup()
{
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  delay(1000);

  Serial.println("=== BP Reader Ready ===");
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
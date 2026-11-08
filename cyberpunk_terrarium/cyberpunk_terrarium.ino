#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN 8
#define SCL_PIN 9

#define ROTATION     U8G2_R3
#define SCROLL_STEP  2
#define FRAME_MS     28

#define JP_FONT    u8g2_font_unifont_t_japanese3
#define LATIN_FONT u8g2_font_9x15B_tr

U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(ROTATION, U8X8_PIN_NONE);

const int W = 32;
const int H = 128;
const int CELL = 16;
const int HEADER_H = 21;

struct Msg { const char *tag; const char *body; };

const Msg msgs[] = {
  { "EX1", "サンプル 一 ここに 文章を 入れます" },
  { "EX2", "これは|~EXAMPLE|~TWO|です" },
  { "EX3", "~EXAMPLE|~THREE|~HELLO|~WORLD" },
};
const int MSG_COUNT = sizeof(msgs) / sizeof(msgs[0]);

const int MAX_IT = 110;
int16_t  itY[MAX_IT];
uint8_t  itKind[MAX_IT];
uint16_t itStart[MAX_IT];
uint8_t  itLen[MAX_IT];
int      nIt = 0;

int      curMsg  = 0;
int      scrollY = H;
int      textH   = 0;
uint32_t lastFrame = 0;

uint32_t nextGlitchAt = 0;
uint32_t glitchUntil  = 0;
bool     glitching    = false;
bool     blackout     = false;
uint8_t  baseContrast = 160;

int utf8Len(uint8_t c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  return 4;
}

void shiftPage(uint8_t *buf, int page, int d) {
  uint8_t *row = buf + page * 128;
  if (d > 0) {
    memmove(row + d, row, 128 - d);
    memset(row, 0, d);
  } else if (d < 0) {
    d = -d;
    memmove(row, row + d, 128 - d);
    memset(row + 128 - d, 0, d);
  }
}

void scheduleGlitch(uint32_t now, bool burst = false) {
  nextGlitchAt = now + (burst ? random(100, 400) : random(1200, 5000));
}

void startGlitch(uint32_t now, uint32_t dur) {
  glitching   = true;
  glitchUntil = now + dur;
  blackout    = false;
}

void loadMsg(int idx) {
  curMsg = idx;
  const char *s = msgs[idx].body;
  nIt = 0;
  int y = 2;
  int i = 0;
  while (s[i] && nIt < MAX_IT - 1) {
    if (s[i] == '|') { i++; y += 6; continue; }
    if (s[i] == '~') {

      i++;
      while (s[i] && s[i] != '|' && nIt < MAX_IT - 1) {
        if (s[i] == ' ') { y += 10; i++; continue; }
        itY[nIt] = y; itKind[nIt] = 1; itStart[nIt] = i; itLen[nIt] = 1;
        nIt++;
        y += CELL;
        i++;
      }
    } else {

      while (s[i] && s[i] != '|' && nIt < MAX_IT - 1) {
        if (s[i] == ' ') { y += 10; i++; continue; }
        int l = utf8Len((uint8_t)s[i]);
        itY[nIt] = y; itKind[nIt] = 0; itStart[nIt] = i; itLen[nIt] = l;
        nIt++;
        y += CELL;
        i += l;
      }
    }
  }
  textH   = y + 6;
  scrollY = H;
}

void drawCorrupted(const char *s, int x, int y, bool corrupt) {
  char tmp[12];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  if (corrupt) {
    const char junk[] = "#%&@$01/\\|<>?";
    int n = strlen(tmp);
    int hits = random(1, 3);
    for (int i = 0; i < hits; i++) {
      int p = random(n);
      tmp[p] = junk[random(sizeof(junk) - 1)];
    }
  }
  u8g2.drawStr(x, y, tmp);
}

void drawBody(int dx, int dy) {
  const char *s = msgs[curMsg].body;
  char buf[16];
  for (int k = 0; k < nIt; k++) {
    int y = scrollY + itY[k] + dy;
    if (y > H || y + CELL < 0) continue;
    if (itKind[k] == 1) {

      buf[0] = s[itStart[k]];
      buf[1] = 0;
      u8g2.setFont(LATIN_FONT);
      int x = (W - u8g2.getStrWidth(buf)) / 2;
      u8g2.drawStr(x + dx, y + 13, buf);
    } else {
      const uint8_t *p = (const uint8_t *)s + itStart[k];

      if (itLen[k] == 3 && p[0] == 0xE3 && p[1] == 0x83 && p[2] == 0xBC) {
        u8g2.drawBox(W / 2 - 1 + dx, y + 1, 2, 14);
        continue;
      }
      memcpy(buf, p, itLen[k]);
      buf[itLen[k]] = 0;
      u8g2.setFont(JP_FONT);
      u8g2.drawUTF8(8 + dx, y + 13, buf);
    }
  }
}

#define SENSOR_EVERY_MS  20000
#define SENSOR_SHOW_MS    7000
#define SENSOR_PRE_MS     1500

uint8_t  shtAddr        = 0x44;
bool     sensorOk       = false;
float    tempC          = 0;
float    humRH          = 0;
uint32_t lastSensorShow = 0;
uint32_t sensorUntil    = 0;
bool     showSensor     = false;
bool     sensorPre      = false;

uint8_t shtCrc(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) {
      c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
  }
  return c;
}

bool readSensor() {
  Wire.beginTransmission(shtAddr);
  Wire.write(0x24);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(16);
  if (Wire.requestFrom((int)shtAddr, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (shtCrc(b, 2) != b[2] || shtCrc(b + 3, 2) != b[5]) return false;
  uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
  tempC = -45.0f + 175.0f * (float)rawT / 65535.0f;
  humRH = 100.0f * (float)rawH / 65535.0f;
  return true;
}

void drawSensorBlock(int y0, const char *label, const char *val, bool isTemp, float frac) {

  u8g2.setFont(JP_FONT);
  u8g2.drawUTF8(0, y0 + 13, label);

  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.drawStr((W - u8g2.getStrWidth(val)) / 2, y0 + 29, val);

  u8g2.setFont(u8g2_font_5x7_tr);
  if (isTemp) {
    u8g2.drawCircle(12, y0 + 33, 1);
    u8g2.drawStr(15, y0 + 38, "C");
  } else {
    u8g2.drawStr(14, y0 + 38, "%");
  }

  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  u8g2.drawFrame(2, y0 + 41, 28, 5);
  u8g2.drawBox(3, y0 + 42, (int)(frac * 26), 3);
}

void renderSensor() {
  char t[10], h[10];
  if (sensorOk) {
    snprintf(t, sizeof(t), "%.1f", tempC);
    snprintf(h, sizeof(h), "%.1f", humRH > 99.9f ? 99.9f : humRH);
  } else {
    strcpy(t, "--.-");
    strcpy(h, "--.-");
  }
  drawSensorBlock(24, "温度", t, true,  sensorOk ? tempC / 40.0f : 0);
  drawSensorBlock(76, "湿度", h, false, sensorOk ? humRH / 100.0f : 0);
}

void drawFrame(uint32_t now) {
  u8g2.clearBuffer();

  if (showSensor) {
    renderSensor();
  } else {

    int ph = (now / 45) % 8;
    for (int y = HEADER_H + 3; y < H; y++) {
      if (((y + ph) & 7) < 3) {
        u8g2.drawPixel(0, y);
        u8g2.drawPixel(W - 1, y);
      }
    }

    drawBody(0, 0);

    if (glitching && random(2) == 0) {
      drawBody(random(-3, 4), random(-2, 3));
    }
  }

  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0, W, HEADER_H);
  u8g2.setDrawColor(1);

  u8g2.setFont(u8g2_font_5x7_tr);
  const char *tag = showSensor ? (sensorOk ? "SENSOR" : "NO SNS") : msgs[curMsg].tag;
  drawCorrupted(tag, 1, 7, glitching && random(3) == 0);

  char hex[8];
  snprintf(hex, sizeof(hex), "%02X%02X", (uint8_t)(now >> 4), (uint8_t)random(256));
  u8g2.drawStr(1, 16, hex);
  if ((now / 400) % 2 == 0) u8g2.drawBox(W - 6, 10, 4, 6);

  u8g2.drawHLine(0, HEADER_H - 2, W);

  if (glitching) {
    uint8_t *buf = u8g2.getBufferPtr();

    int bands = random(1, 4);
    for (int i = 0; i < bands; i++) {
      shiftPage(buf, random(4), random(-30, 31));
    }

    u8g2.setDrawColor(2);
    int boxes = random(1, 4);
    for (int i = 0; i < boxes; i++) {
      u8g2.drawBox(random(0, 24), random(0, H - 8), random(6, 32), random(1, 8));
    }
    u8g2.setDrawColor(1);

    int noise = random(20, 100);
    for (int i = 0; i < noise; i++) {
      u8g2.drawPixel(random(W), random(H));
    }

    if (random(4) == 0) {
      u8g2.setDrawColor(2);
      u8g2.drawHLine(0, random(H), W);
      u8g2.setDrawColor(1);
    }

    u8g2.setContrast(random(2) ? 255 : random(5, 60));
  } else {
    u8g2.setContrast(baseContrast);
  }

  if (blackout) u8g2.clearBuffer();

  u8g2.sendBuffer();
}

void bootSequence() {
  const char *boot = "起動中";
  for (int i = 0; i < 18; i++) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(1, 7, "BOOT");
    u8g2.drawStr(1, 16, "v2.077");
    u8g2.setFont(JP_FONT);
    char one[4];
    for (int c = 0; c < 3; c++) {
      memcpy(one, boot + c * 3, 3);
      one[3] = 0;
      u8g2.drawUTF8(8, 48 + c * 18, one);
    }
    uint8_t *buf = u8g2.getBufferPtr();
    for (int b = 0; b < 3; b++) shiftPage(buf, random(4), random(-35, 36));
    for (int n = 0; n < 150; n++) u8g2.drawPixel(random(W), random(H));
    u8g2.setContrast(random(2) ? 255 : 20);
    u8g2.sendBuffer();
    delay(70);
  }
}

#define FLUORO_PIN       0
#define FLUORO_PWM_FREQ  2000
#define FLUORO_BASE      230

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define FLUORO_ATTACH() ledcAttach(FLUORO_PIN, FLUORO_PWM_FREQ, 8)
  #define FLUORO_OUT(v)   ledcWrite(FLUORO_PIN, (v))
#else
  #define FLUORO_ATTACH() do { ledcSetup(0, FLUORO_PWM_FREQ, 8); ledcAttachPin(FLUORO_PIN, 0); } while (0)
  #define FLUORO_OUT(v)   ledcWrite(0, (v))
#endif

uint32_t fluoroNextAt      = 0;
uint32_t fluoroNextBurstAt = 0;
int      fluoroSteps       = 0;
bool     fluoroStrike      = false;

void fluoroInit() {
  FLUORO_ATTACH();
  FLUORO_OUT(0);
  fluoroStrike = true;
  fluoroSteps  = random(14, 22);
  fluoroNextAt = millis();
}

void fluoroUpdate(uint32_t now) {
  if ((int32_t)(now - fluoroNextAt) < 0) return;

  if (fluoroSteps > 0) {
    fluoroSteps--;
    int lvl;
    if (fluoroStrike) {

      int chance = 100 - fluoroSteps * 5;
      if (chance < 20) chance = 20;
      if (random(100) < chance)  lvl = random(150, 256);
      else if (random(3) == 0)   lvl = 0;
      else                       lvl = random(10, 70);
      if (fluoroSteps == 0) lvl = FLUORO_BASE;
    } else {

      int r = random(100);
      if (r < 45)       lvl = 0;
      else if (r < 75)  lvl = random(20, 100);
      else              lvl = random(150, 256);
    }
    FLUORO_OUT(lvl);
    fluoroNextAt = now + random(15, 90);
    if (fluoroSteps == 0) {
      fluoroStrike = false;
      fluoroNextBurstAt = now + random(4000, 25000);
    }
    return;
  }

  if (now >= fluoroNextBurstAt) {
    if (random(5) == 0) {

      FLUORO_OUT(0);
      fluoroStrike = true;
      fluoroSteps  = random(5, 11);
      fluoroNextAt = now + random(200, 700);
    } else {
      fluoroStrike = false;
      fluoroSteps  = random(3, 9);
      fluoroNextAt = now;
    }
    return;
  }

  int lvl = FLUORO_BASE + random(-12, 6);
  if (lvl > 255) lvl = 255;
  FLUORO_OUT(lvl);
  fluoroNextAt = now + random(40, 120);
}

#define NEON_PIN   21
#define NEON_BASE  200
#define BEACON_PIN       20
#define BEACON_PERIOD    1600
#define BEACON_MAX       255

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define PWM_ATTACH(pin, ch) ledcAttach((pin), 2000, 8)
  #define PWM_OUT(pin, ch, v) ledcWrite((pin), (v))
#else
  #define PWM_ATTACH(pin, ch) do { ledcSetup((ch), 2000, 8); ledcAttachPin((pin), (ch)); } while (0)
  #define PWM_OUT(pin, ch, v) ledcWrite((ch), (v))
#endif

uint32_t neonNextAt = 0;
int      neonLast   = -1;
int      beaconLast = -1;

void neonSet(int v) {
  if (v != neonLast) { PWM_OUT(NEON_PIN, 1, v); neonLast = v; }
}

void extraLedsInit() {
  PWM_ATTACH(NEON_PIN, 1);
  PWM_ATTACH(BEACON_PIN, 2);
  neonSet(NEON_BASE);
  PWM_OUT(BEACON_PIN, 2, 0);
}

void neonUpdate(uint32_t now) {
  if (glitching) {
    if (blackout) { neonSet(0); return; }
    if ((int32_t)(now - neonNextAt) < 0) return;
    int r = random(100);
    int lvl = (r < 35) ? 0 : (r < 65) ? random(30, 120) : random(180, 256);
    neonSet(lvl);
    neonNextAt = now + random(8, 40);
  } else {
    neonSet(NEON_BASE);
  }
}

void beaconUpdate(uint32_t now) {
  uint32_t t = now % BEACON_PERIOD;
  int lvl;
  if (t < 80) {
    lvl = BEACON_MAX * (int)t / 80;
  } else if (t < 300) {
    lvl = BEACON_MAX;
  } else if (t < 600) {
    float x = (float)(t - 300) / 300.0f;
    lvl = (int)(BEACON_MAX * (1.0f - x) * (1.0f - x));
  } else {
    lvl = 0;
  }
  if (lvl != beaconLast) { PWM_OUT(BEACON_PIN, 2, lvl); beaconLast = lvl; }
}

void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  fluoroInit();
  extraLedsInit();
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(baseContrast);

  randomSeed(esp_random());

  Wire.beginTransmission(0x44);
  if (Wire.endTransmission() == 0) {
    shtAddr = 0x44;
  } else {
    Wire.beginTransmission(0x45);
    if (Wire.endTransmission() == 0) shtAddr = 0x45;
  }
  sensorOk = readSensor();

  bootSequence();
  loadMsg(random(MSG_COUNT));
  scheduleGlitch(millis());

  lastSensorShow = millis() - SENSOR_EVERY_MS + 10000;
}

void loop() {
  uint32_t now = millis();
  neonUpdate(now);
  beaconUpdate(now);
  fluoroUpdate(now);
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  if (!glitching && now >= nextGlitchAt) {
    startGlitch(now, random(60, 260));
  }
  if (glitching) {
    blackout = (random(14) == 0);
    if (now >= glitchUntil) {
      glitching = false;
      blackout  = false;
      scheduleGlitch(now, random(4) == 0);
    }
  }

  if (showSensor) {

    if (now >= sensorUntil) {
      showSensor = false;
      lastSensorShow = now;
      startGlitch(now, 260);
    }
  } else {
    bool due = (now - lastSensorShow >= SENSOR_EVERY_MS);

    const int preEndPx = SCROLL_STEP * (SENSOR_PRE_MS / FRAME_MS);
    if (due && !sensorPre && (scrollY + textH) <= preEndPx) {
      sensorOk = readSensor();
      sensorPre = true;
    }

    scrollY -= SCROLL_STEP;
    if (scrollY < -textH) {

      int next;
      do { next = random(MSG_COUNT); } while (next == curMsg && MSG_COUNT > 1);
      loadMsg(next);
      if (sensorPre) {
        sensorPre   = false;
        showSensor  = true;
        sensorUntil = now + SENSOR_SHOW_MS;
        startGlitch(now, 260);
      } else {
        startGlitch(now, 220);
      }
    }
  }

  drawFrame(now);
}

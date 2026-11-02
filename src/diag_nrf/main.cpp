#include <Arduino.h>

/*
 * Diagnosis nRF24L01 di Ground Station (pin sama dengan src/ground_station).
 *
 *   Upload : pio run -e gs_diag -t upload
 *   Monitor: pio device monitor -e gs_diag        (115200)
 *   Selesai: upload lagi env ground_station
 *
 * Setiap 1 detik: baca register nRF lewat SPI (MISO dengan pull-up lalu pull-down untuk
 * mendeteksi pin mengambang), uji tulis register RF_CH, lalu RF24.begin(). Hasil + dugaan
 * kabel yang bermasalah tampil di serial dan OLED. LED hijau = nRF menjawab, merah = gagal.
 * Goyangkan kabel satu per satu sambil melihat OLED untuk menemukan sambungan longgar.
 */

#include <SPI.h>
#include <Wire.h>
#include <RF24.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/gpio.h>

#define PIN_SCK 18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_CSN 5
#define PIN_CE 4
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BUZZER 2
#define PIN_LED_G 27
#define PIN_LED_R 15
#define OLED_ADDR 0x3C

#define R_REGISTER 0x00
#define W_REGISTER 0x20
#define REG_CONFIG 0x00
#define REG_SETUP_AW 0x03
#define REG_RF_CH 0x05
#define REG_RF_SETUP 0x06

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;
RF24 radio(PIN_CE, PIN_CSN, 1000000);
SPISettings spiCfg(500000, MSBFIRST, SPI_MODE0);

struct Regs
{
  uint8_t status, config, aw, ch, rfSetup;
};

uint8_t xfer2(uint8_t cmd, uint8_t data, uint8_t *status)
{
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  delayMicroseconds(5);
  uint8_t st = SPI.transfer(cmd);
  uint8_t v = SPI.transfer(data);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
  if (status)
    *status = st;
  return v;
}

uint8_t readReg(uint8_t reg, uint8_t *status = nullptr)
{
  return xfer2(R_REGISTER | (reg & 0x1F), 0xFF, status);
}

void writeReg(uint8_t reg, uint8_t v)
{
  xfer2(W_REGISTER | (reg & 0x1F), v, nullptr);
}

Regs readAll()
{
  Regs r;
  r.config = readReg(REG_CONFIG, &r.status);
  r.aw = readReg(REG_SETUP_AW);
  r.ch = readReg(REG_RF_CH);
  r.rfSetup = readReg(REG_RF_SETUP);
  return r;
}

bool sameRegs(const Regs &a, const Regs &b)
{
  return a.status == b.status && a.config == b.config && a.aw == b.aw && a.ch == b.ch &&
         a.rfSetup == b.rfSetup;
}

bool allValue(const Regs &r, uint8_t v)
{
  return r.status == v && r.config == v && r.aw == v && r.ch == v && r.rfSetup == v;
}

// Register masuk akal untuk nRF24L01: SETUP_AW 1..3, STATUS bit7 selalu 0
bool plausible(const Regs &r)
{
  return r.aw >= 1 && r.aw <= 3 && !(r.status & 0x80) && r.ch <= 0x7F;
}

void setup()
{
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  digitalWrite(PIN_LED_G, LOW);
  digitalWrite(PIN_LED_R, LOW);
  pinMode(PIN_CE, OUTPUT);
  digitalWrite(PIN_CE, LOW); // standby: register boleh ditulis
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CSN, HIGH);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== DIAGNOSIS nRF24 GROUND STATION ===");
  Serial.printf("Pin: CE=%d CSN=%d SCK=%d MOSI=%d MISO=%d  (VCC ke 3V3, GND ke GND)\n",
                PIN_CE, PIN_CSN, PIN_SCK, PIN_MOSI, PIN_MISO);

  Wire.begin(PIN_SDA, PIN_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk)
  {
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);
  }
  else
  {
    Serial.println("OLED tidak terdeteksi (hasil hanya di serial)");
  }

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  delay(100); // nRF butuh ~100 ms setelah daya masuk
}

void loop()
{
  static uint32_t cycle = 0;
  static Regs prev = {};
  static bool havePrev = false;
  cycle++;

  // 1. baca dengan pull-up lalu pull-down di MISO: beda = MISO tidak digerakkan nRF
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_PULLUP_ONLY);
  delay(2);
  Regs up = readAll();
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_PULLDOWN_ONLY);
  delay(2);
  Regs dn = readAll();
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_FLOATING);

  // 2. uji tulis RF_CH (butuh MOSI)
  uint8_t orig = dn.ch & 0x7F;
  writeReg(REG_RF_CH, 0x2A);
  uint8_t w1 = readReg(REG_RF_CH);
  writeReg(REG_RF_CH, 0x55);
  uint8_t w2 = readReg(REG_RF_CH);
  writeReg(REG_RF_CH, orig);
  bool writeOk = (w1 == 0x2A && w2 == 0x55);

  // 3. library RF24 (seperti firmware GS)
  bool libOk = radio.begin(&SPI);
  bool pVariant = libOk && radio.isPVariant();

  // 4. dugaan
  const char *verdict, *hint;
  bool ok = false;
  bool floating = !sameRegs(up, dn);
  bool changed = havePrev && !sameRegs(prev, dn);
  if (floating)
  {
    verdict = "nRF TIDAK menjawab";
    hint = "MISO mengambang: cek VCC 3V3+GND nRF, CSN->5, SCK->18, MISO->19";
  }
  else if (allValue(dn, 0x00))
  {
    verdict = "MISO selalu 0";
    hint = "MISO korslet ke GND / nRF tanpa daya / modul rusak";
  }
  else if (allValue(dn, 0xFF))
  {
    verdict = "MISO selalu 1";
    hint = "MISO korslet ke 3V3 / modul rusak";
  }
  else if (!plausible(dn))
  {
    verdict = "data acak";
    hint = "SCK/CSN longgar atau daya nRF tidak stabil (pasang kapasitor 10-100uF)";
  }
  else if (!writeOk)
  {
    verdict = "baca OK, tulis GAGAL";
    hint = "cek MOSI->23 (dan CE->4 harus LOW)";
  }
  else
  {
    ok = true;
    verdict = changed ? "nRF OK tapi nilai berubah" : "nRF OK";
    hint = changed ? "sambungan longgar? goyangkan kabel" : "sambungan SPI baik";
  }
  prev = dn;
  havePrev = true;

  digitalWrite(PIN_LED_G, ok ? HIGH : LOW);
  digitalWrite(PIN_LED_R, ok ? LOW : HIGH);

  Serial.printf("[%lu] STATUS=%02X CONFIG=%02X SETUP_AW=%02X RF_CH=%02X RF_SETUP=%02X | pull-down: %02X %02X %02X %02X %02X | "
                "tulis RF_CH: 2A->%02X 55->%02X | RF24.begin %s%s\n",
                (unsigned long)cycle, up.status, up.config, up.aw, up.ch, up.rfSetup,
                dn.status, dn.config, dn.aw, dn.ch, dn.rfSetup, w1, w2, libOk ? "OK" : "GAGAL",
                libOk ? (pVariant ? " (nRF24L01+, 250kbps bisa)" : " (bukan +, 250kbps TIDAK bisa)") : "");
  Serial.printf("    => %s: %s\n", verdict, hint);

  if (oledOk)
  {
    oled.clearDisplay();
    oled.fillRect(0, 0, 128, 9, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(2, 1);
    oled.printf("DIAG nRF24     #%lu", (unsigned long)cycle);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 11);
    oled.printf("ST %02X CFG %02X AW %02X", dn.status, dn.config, dn.aw);
    oled.setCursor(0, 20);
    oled.printf("CH %02X RFS %02X %s", dn.ch, dn.rfSetup, floating ? "MISO?" : "");
    oled.setCursor(0, 29);
    oled.printf("tulis %s  lib %s", writeOk ? "OK" : "GAGAL", libOk ? (pVariant ? "OK+" : "OK") : "GAGAL");
    oled.setCursor(0, 40);
    oled.print(verdict);
    // petunjuk dipotong 2 baris x 21 karakter
    char line[22];
    for (int k = 0; k < 2; k++)
    {
      strncpy(line, hint + k * 21 < hint + strlen(hint) ? hint + k * 21 : "", 21);
      line[21] = 0;
      oled.setCursor(0, 49 + k * 8);
      oled.print(line);
    }
    oled.display();
  }
  delay(1000);
}

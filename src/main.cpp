// ============================================================
//  Rownd CNC Handwheel — ESP32 USB-Serial Firmware
//  Donanım: HW-040 Encoder + 1.44" ST7735 TFT (SPI, 128x128)
//  Hedef:   ESP32 → USB → Pi → bridge.js → cncjs
//
//  Encoder Bağlantısı:
//    HW-040 GND → ESP32 GND
//    HW-040  +  → ESP32 3.3V
//    HW-040 CLK → GPIO 26
//    HW-040  DT → GPIO 27
//    HW-040  SW → GPIO 4
//
//  TFT Bağlantısı (Donanım SPI):
//    ST7735 GND → ESP32 GND
//    ST7735 VCC → ESP32 3.3V
//    ST7735 SDA → GPIO 23  (ESP32 MOSI)
//    ST7735 SCL → GPIO 18  (ESP32 SCLK)
//    ST7735 CS  → GPIO 5
//    ST7735 DC  → GPIO 2
//    ST7735 RES → GPIO 15
// ============================================================
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "esp_system.h"

// ── TFT Pin Tanımları (1.44" ST7735, 128x128) ──────────────
//
// SECENEK A — Donanım SPI (Hızlı, önerilen):
//   Modülün "SDA" pini → GPIO 23 (ESP32 MOSI)
//   Modülün "SCL" pini → GPIO 18 (ESP32 SCLK)
//
// SECENEK B — Yazılım SPI (GPIO 21/22 kullanmak istersen):
//   Modülün "SDA" pini → GPIO 21 (ESP32'nin I2C SDA pini, SPI olarak kullan)
//   Modülün "SCL" pini → GPIO 22 (ESP32'nin I2C SCL pini, SPI olarak kullan)
//   MOSI/SCLK satırlarını uncomment et, Adafruit_ST7735 çağrısını değiştir

#define TFT_CS    5    // Chip Select
#define TFT_DC    2    // Data/Command (DC / A0 / RS)
#define TFT_RST   15   // Reset

// SECENEK A — Donanım SPI (MOSI=GPIO23, SCLK=GPIO18 otomatik kullanılır)
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// SECENEK B — Yazılım SPI (GPIO 21/22 istersen)
// #define TFT_MOSI  21   // Modülün SDA/DIN pini
// #define TFT_SCLK  22   // Modülün SCL pini
// Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Renk sabitleri
#define COLOR_BG       ST77XX_BLACK
#define COLOR_LABEL    0x07FF   // Cyan
#define COLOR_VALUE    ST77XX_WHITE
#define COLOR_STATE    ST77XX_WHITE
#define COLOR_ALARM    ST77XX_RED
#define COLOR_JOG      ST77XX_YELLOW
#define COLOR_HEADER   0x4208   // Koyu gri

// ── Encoder Pin Tanımları ────────────────────────────────────
#define ENC_CLK_PIN   26   // HW-040 CLK (A fazı)
#define ENC_DT_PIN    27   // HW-040 DT  (B fazı)
#define ENC_SW_PIN    4    // HW-040 SW  (Buton — ileride eksen/mod seçimi)

// ── Potansiyometre Pin Tanımları ─────────────────────────────
#define AXIS_POT_PIN  34   // Eksen seçim potansiyometresi (OFF, X, Y, Z)
#define MULT_POT_PIN  35   // Çözünürlük seçim potansiyometresi (0.01, 0.1, 1.0)

// ── Protokol Sabitleri ───────────────────────────────────────
#define SERIAL_BAUD_RATE        115200  // bridge.js ile eşleşmeli

// ── Watchdog (Kalp Atışı) ────────────────────────────────────
// bridge.js, 600ms sessizlik → JogCancel (0x85) atar.
// Biz her 200ms'de bir '?' göndererek "hayattayım" deriz.
#define HEARTBEAT_INTERVAL_MS   200

// ── Accumulator Ayarları ─────────────────────────────────────
#define JOG_INTERVAL_MS         150    // Paket yollama periyodu (ms)
#define JOG_FEED_RATE           300    // Jog besleme hızı (mm/dak)

// ── Dinamik Ayarlar (Potansiyometrelerden okunur) ────────────
static float currentStepMM    = 0.01f;
static char  selectedAxis     = 'O';   // 'O'=OFF, 'X', 'Y', 'Z'
static float lastStepMM       = -1.0f; // UI güncelleme takibi
static char  lastSelectedAxis = 'U';   // UI güncelleme takibi

// ── Debounce ─────────────────────────────────────────────────
#define DEBOUNCE_DELAY_MS       50

// ── Ekran Hazır Bayrağı ────────────────────────────────
// İlk updateDisplay() çağrısında hoşgeldin ekranını silip temiz başlat.
// Sonraki çağrılarda partial fillRect ile güncelleme yapılır.
static bool tftReady = false;

// ── Encoder ISR Değişkenleri ─────────────────────────────────
// Her fiziksel tıkta quadrature state machine 4 geçiş üretir.
// Gerçek detent sayısı = encoderRaw / 4
// Detent bölme işlemi loop'ta yapılır, ISR'de değil.
volatile int32_t encoderRaw = 0;

// ── Timing ───────────────────────────────────────────────────
static unsigned long lastPacketTime    = 0;
static unsigned long lastHeartbeatTime = 0;
static unsigned long lastDebounceTime  = 0;
static int           lastButtonState   = HIGH;
static int           stableButtonState = HIGH;

// ── Makine Durumu ─────────────────────────────────────────────
// Bridge.js → ESP32 yönünde gelen durum raporundan parse edilir.
struct MachineStatus {
    char  state[12];  // "Idle", "Jog", "Run", "Alarm", "Hold"...
    float x, z, c;
    bool  valid;      // İlk geçerli veri gelene kadar false
};
static MachineStatus machineStatus = { "---", 0.0f, 0.0f, 0.0f, false };

// ── Gelen Satır Buffer'ı ─────────────────────────────────────
// Bridge.js'den gelen "ok\n", "<Idle|MPos:...>\n" vb. satırlar
static char   rxBuf[128];
static uint8_t rxLen = 0;

// ─────────────────────────────────────────────────────────────
// ENCODER ISR — Quadrature State Machine
// ─────────────────────────────────────────────────────────────
void IRAM_ATTR handleEncoderChange() {
    static uint8_t old_AB = 0;
    uint8_t A = digitalRead(ENC_CLK_PIN);
    uint8_t B = digitalRead(ENC_DT_PIN);

    // 2 bitlik mevcut durum → 4 bitlik geçiş kodu
    uint8_t current_AB = (A << 1) | B;
    uint8_t state      = (old_AB << 2) | current_AB;

    // Geçerli CW geçişleri
    if (state == 0b1101 || state == 0b0100 ||
        state == 0b0010 || state == 0b1011) {
        encoderRaw++;
    }
    // Geçerli CCW geçişleri
    else if (state == 0b1110 || state == 0b0111 ||
             state == 0b0001 || state == 0b1000) {
        encoderRaw--;
    }
    // Diğer tüm geçişler (bounce/skip): yok say

    old_AB = current_AB;
}

// ─────────────────────────────────────────────────────────────
// STATUS PARSER
// Grbl/FluidNC durum satırı: <State|MPos:X,Y,Z,A|FS:f,s|R:m>
// ─────────────────────────────────────────────────────────────
void parseStatusLine(const char* line) {
    if (line[0] != '<') return;

    // State bölümü: '<' ile '|' arasındaki metin
    const char* stateEnd = strchr(line + 1, '|');
    if (!stateEnd) return;
    size_t stateLen = (size_t)(stateEnd - (line + 1));
    if (stateLen >= sizeof(machineStatus.state))
        stateLen = sizeof(machineStatus.state) - 1;
    strncpy(machineStatus.state, line + 1, stateLen);
    machineStatus.state[stateLen] = '\0';

    // MPos: X,Y,Z,...
    const char* mpos = strstr(line, "MPos:");
    if (!mpos) return;
    mpos += 5;  // "MPos:" uzunluğu

    machineStatus.x = atof(mpos);
    const char* c1 = strchr(mpos, ',');
    if (!c1) return;
    machineStatus.z = atof(c1 + 1); // 2. değer (Z)
    const char* c2 = strchr(c1 + 1, ',');
    if (!c2) return;
    machineStatus.c = atof(c2 + 1); // 3. değer (C)

    machineStatus.valid = true;
}

// ─────────────────────────────────────────────────────────────
// TFT EKRAN GÜNCELLEMESİ (ST7735 128x160)
// ─────────────────────────────────────────────────────────────
// Layout:
//   y=  0–18 : Başlık şeridi
//   y= 22–46 : Makine durumu (text 2)
//   y= 48    : Ayırıcı
//   y= 54–74 : X koordinatı  (text 2)
//   y= 80–100: Y koordinatı  (text 2)
//   y=106–126: Z koordinatı  (text 2)
//   y=130    : Ayırıcı
//   y=134–142: Step bilgisi  (text 1)
//   y=148–156: Feed hızı     (text 1)
// ─────────────────────────────────────────────────────────────
void updateDisplay() {
    char buf[20];

    // İlk çağrıda hoşgeldin ekranını tamamen sil, sabit çerçeveyi çiz
    if (!tftReady) {
        tft.fillScreen(ST77XX_BLACK);
        // Sabit ayırıcı çizgiler
        tft.drawFastHLine(0, 48, 128, 0x39E7);
        tft.drawFastHLine(0, 130, 128, 0x39E7);
        tftReady = true;
    }

    // ── Başlık Şeridi (Dinamik) ──────────────────────────────
    static char lastHeaderState = 'U';
    if (selectedAxis != lastHeaderState) {
        tft.fillRect(0, 0, 128, 18, COLOR_HEADER);
        tft.setTextSize(1);
        tft.setCursor(4, 5);
        if (selectedAxis == 'O') {
            tft.setTextColor(ST77XX_RED);
            tft.print("HANDWHEEL: OFF");
        } else {
            tft.setTextColor(0xAD75);
            tft.print("Rownd CNC Handwheel");
        }
        lastHeaderState = selectedAxis;
    }

    // Durum rengi
    uint16_t stateColor = ST77XX_WHITE;
    if      (strncmp(machineStatus.state, "Alarm", 5) == 0) stateColor = ST77XX_RED;
    else if (strncmp(machineStatus.state, "Jog",   3) == 0) stateColor = ST77XX_YELLOW;
    else if (strncmp(machineStatus.state, "Run",   3) == 0) stateColor = ST77XX_YELLOW;
    else if (strncmp(machineStatus.state, "Hold",  4) == 0) stateColor = 0xFD20;

    // ── Makine Durumu (y=20, h=26) ───────────────────────────
    tft.fillRect(0, 20, 128, 26, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(stateColor);
    tft.setCursor(4, 22);
    if (machineStatus.valid) {
        tft.print(machineStatus.state);
    } else {
        tft.setTextColor(0x8410);
        tft.print("Bekliyor..");
    }

    // ── X / Z / C Koordinatları ve Seçili Eksen Vurgusu ──────
    const float vals[3] = { machineStatus.x, machineStatus.z, machineStatus.c };
    const char axesChars[3] = { 'X', 'Z', 'C' };
    
    for (int i = 0; i < 3; i++) {
        uint8_t y = 54 + i * 26;
        bool isActive = (selectedAxis == axesChars[i]);
        
        // Aktif ise lacivert (0x01CF) arka plan, değilse siyah
        uint16_t bgColor = isActive ? 0x01CF : ST77XX_BLACK;
        uint16_t labelColor = isActive ? ST77XX_YELLOW : COLOR_LABEL;
        uint16_t valColor = isActive ? ST77XX_WHITE : 0xAD75;

        // Tüm satırı (128x22) arka plan rengiyle boya (highlight)
        tft.fillRect(0, y, 128, 22, bgColor);

        tft.setTextSize(2);
        tft.setTextColor(labelColor);
        
        // Metni dikeyde ortalamak için y+4 kullanıyoruz (Kutu h=22, Metin h=14)
        tft.setCursor(isActive ? 0 : 4, y + 4);
        if (isActive) tft.print(">");
        tft.print(axesChars[i]);
        tft.print(":");

        snprintf(buf, sizeof(buf), "%8.3f", vals[i]);
        tft.setTextColor(valColor);
        tft.setCursor(28, y + 4);
        tft.print(buf);
    }

    // ── Alt bilgi: Step/Feed ─────────────────────────────────
    static float lastPrintedStep = -1.0f;
    if (currentStepMM != lastPrintedStep) {
        tft.fillRect(0, 132, 128, 28, ST77XX_BLACK);
        tft.setTextSize(1);
        tft.setTextColor(0xAD75);
        tft.setCursor(4, 136);
        snprintf(buf, sizeof(buf), "Step : %.2f mm", currentStepMM);
        tft.print(buf);
        tft.setCursor(4, 148);
        snprintf(buf, sizeof(buf), "Feed : %d mm/min", JOG_FEED_RATE);
        tft.print(buf);
        lastPrintedStep = currentStepMM;
    }
}



// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD_RATE);

    // TFT başlat (1.8" / 128x160 ST7735)
    tft.initR(INITR_BLACKTAB);  // 128x160 modüller için BLACKTAB
    tft.setRotation(0);          // 0-3 arası, ekran yönüne göre ayarla
    tft.fillScreen(COLOR_BG);

    // Hoşgeldin ekranı (128x160 ortasına yerleştirilmiş)
    tft.setTextSize(2);
    tft.setTextColor(COLOR_LABEL);
    tft.setCursor(8, 50);
    tft.print("Rownd CNC");
    tft.setTextSize(1);
    tft.setTextColor(0xAD75);
    tft.setCursor(18, 76);
    tft.print("Handwheel v1.0");
    tft.setCursor(26, 92);
    tft.print("Baslaniyor...");

    // Boot nedenini bridge.js'e bildir
    delay(200);
    Serial.print("@RST ");
    Serial.print((int)esp_reset_reason());
    Serial.print("\n");

    // Encoder pinleri
    pinMode(ENC_CLK_PIN, INPUT_PULLUP);
    pinMode(ENC_DT_PIN,  INPUT_PULLUP);
    pinMode(ENC_SW_PIN,  INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), handleEncoderChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT_PIN),  handleEncoderChange, CHANGE);
}

// ── Potansiyometre Filtresi (Ortalama ve Hysteresis) ──────────
int readAnalogFiltered(uint8_t pin) {
    long sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(pin);
    }
    return sum / 16;
}

char getAxisState(int adcVal, char currentState) {
    const int margin = 80; // Titremeyi önlemek için tolerans payı
    
    if (currentState == 'O') {
        if (adcVal > 1024 + margin) return 'X';
    } else if (currentState == 'X') {
        if (adcVal < 1024 - margin) return 'O';
        if (adcVal > 2048 + margin) return 'Z';
    } else if (currentState == 'Z') {
        if (adcVal < 2048 - margin) return 'X';
        if (adcVal > 3072 + margin) return 'C';
    } else if (currentState == 'C') {
        if (adcVal < 3072 - margin) return 'Z';
    }
    
    // İlk okuma
    if (currentState == 'U') {
        if      (adcVal < 1024) return 'O';
        else if (adcVal < 2048) return 'X';
        else if (adcVal < 3072) return 'Z';
        else                    return 'C';
    }
    return currentState;
}

float getMultState(int adcVal, float currentMult) {
    const int margin = 80;
    
    if (currentMult == 0.01f) {
        if (adcVal > 1365 + margin) return 0.10f;
    } else if (currentMult == 0.10f) {
        if (adcVal < 1365 - margin) return 0.01f;
        if (adcVal > 2730 + margin) return 1.00f;
    } else if (currentMult == 1.00f) {
        if (adcVal < 2730 - margin) return 0.10f;
    }
    
    // İlk okuma
    if (currentMult < 0.0f) {
        if      (adcVal < 1365) return 0.01f;
        else if (adcVal < 2730) return 0.10f;
        else                    return 1.00f;
    }
    return currentMult;
}

// ─────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── 0. POTANSİYOMETRE OKUMA ───────────────────────────────
    // Gürültüyü önlemek için ortalama alınır ve eşiklerde hysteresis uygulanır
    int axisVal = readAnalogFiltered(AXIS_POT_PIN);
    selectedAxis = getAxisState(axisVal, lastSelectedAxis);

    int multVal = readAnalogFiltered(MULT_POT_PIN);
    currentStepMM = getMultState(multVal, lastStepMM);

    // Eğer potansiyometrelerden biri değişirse ekranı beklemeden güncelle
    if (selectedAxis != lastSelectedAxis || currentStepMM != lastStepMM) {
        if (tftReady) { // Sadece ekran hazır olduktan sonra
            updateDisplay();
        }
        lastSelectedAxis = selectedAxis;
        lastStepMM = currentStepMM;
    }

    // ── 1. WATCHDOG ───────────────────────────────────────────
    if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
        Serial.write('?');
        lastHeartbeatTime = now;
    }

    // ── 2. SW BUTON DEBOUNCE ─────────────────────────────────
    int rawSW = digitalRead(ENC_SW_PIN);
    if (rawSW != lastButtonState) lastDebounceTime = now;
    if ((now - lastDebounceTime) > DEBOUNCE_DELAY_MS && rawSW != stableButtonState) {
        stableButtonState = rawSW;
        if (stableButtonState == LOW) {
            // İleride: JogCancel, eksen seçimi vb.
        }
    }
    lastButtonState = rawSW;

    // ── 3. ENCODER → JOG PAKETI ──────────────────────────────
    if (now - lastPacketTime >= JOG_INTERVAL_MS) {
        lastPacketTime = now;

        // ISR'den atomik okuma: kısa süreliğine interrupt'ları durdur
        noInterrupts();
        int32_t raw = encoderRaw;
        encoderRaw  = 0;
        interrupts();

        int32_t detents   = raw / 2;
        int32_t remainder = raw % 2;

        noInterrupts();
        encoderRaw += remainder;
        interrupts();

        if (detents != 0 && selectedAxis != 'O') {
            float distance = detents * currentStepMM;

            // $J= : Grbl jogging komutu
            //   G91 = artımlı mod, G21 = mm birimi
            //   Sonuna \n OLMALI (standart G-code satırı)
            //   CW → detents pozitif → eksen pozitif yönde
            //   CCW → detents negatif → eksen negatif yönde
            String cmd = "$J=G91G21" + String(selectedAxis) + String(distance, 2) + "F" + String(JOG_FEED_RATE) + "\n";
            Serial.print(cmd);
        }
    }

    // ── 4. GELİŞ PARSER → OLED ───────────────────────────────
    // Bridge.js'den gelen satırları biriktir, tam satır gelince parse et.
    // Grbl durum satırı: <State|MPos:X,Y,Z,...|FS:...|R:...>
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (rxLen > 0) {
                rxBuf[rxLen] = '\0';

                // Sadece '<' ile başlayan durum satırlarını parse et
                if (rxBuf[0] == '<') {
                    parseStatusLine(rxBuf);
                    updateDisplay();
                }
                rxLen = 0;
            }
        } else if (rxLen < (uint8_t)(sizeof(rxBuf) - 1)) {
            rxBuf[rxLen++] = c;
        } else {
            // Buffer taşması — sıfırla (kötü data)
            rxLen = 0;
        }
    }
}
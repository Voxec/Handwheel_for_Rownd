// ============================================================
//  Rownd CNC Handwheel — HW-040 Encoder Entegrasyon Taslağı
//
//  ⚠️  BU DOSYA DERLENMİYOR — src/ dışında, variants/ klasöründe.
//  Test hazır olduğunda içeriği main.cpp ile birleştir.
//
//  Donanım: HW-040 (KY-040) Rotary Encoder
//
//  Bağlantı:
//    HW-040 GND  → ESP32 GND
//    HW-040  +   → ESP32 3.3V  (⚠️ 5V değil!)
//    HW-040 CLK  → GPIO 18   (A fazı, interrupt)
//    HW-040  DT  → GPIO 19   (B fazı)
//    HW-040  SW  → GPIO 4    (Buton, INPUT_PULLUP)
// ============================================================
#include <Arduino.h>
#include "esp_system.h"

// ── Encoder Pin Tanımları ────────────────────────────────────
#define ENC_CLK_PIN   18   // HW-040 CLK (A fazı)
#define ENC_DT_PIN    19   // HW-040 DT  (B fazı)
#define ENC_SW_PIN    4    // HW-040 SW  (Buton)

// ── Protokol Sabitleri ───────────────────────────────────────
#define SERIAL_BAUD_RATE        115200
#define HEARTBEAT_INTERVAL_MS   200

// ── Accumulator Ayarları ─────────────────────────────────────
#define JOG_INTERVAL_MS         150
#define JOG_STEP_MM             0.05f  // Encoder için daha küçük adım
#define JOG_FEED_RATE           300

// ── Debounce (Buton SW için) ─────────────────────────────────
#define DEBOUNCE_DELAY_MS       50

// ── Encoder ISR Değişkenleri ─────────────────────────────────
// volatile: ISR ve ana döngü arasında güvenli paylaşım için şart
volatile int32_t encoderTicks = 0;
volatile uint8_t lastCLK      = HIGH;

// ── Genel Durum Değişkenleri ─────────────────────────────────
static unsigned long lastPacketTime    = 0;
static unsigned long lastHeartbeatTime = 0;
static unsigned long lastDebounceTime  = 0;
static int           lastButtonState   = HIGH;
static int           stableButtonState = HIGH;

// ─────────────────────────────────────────────────────────────
// ENCODER ISR — CLK pinindeki her değişimde tetiklenir.
//
// HW-040 Quadrature:
//   CW  çevirme: CLK düşerken DT=HIGH → ticks++
//   CCW çevirme: CLK düşerken DT=LOW  → ticks--
//
// IRAM_ATTR: ISR'nin Flash'ta değil IRAM'da çalışmasını sağlar.
//            Cache miss olasılığını ortadan kaldırır.
// ─────────────────────────────────────────────────────────────
void IRAM_ATTR encoderISR() {
    uint8_t clk = digitalRead(ENC_CLK_PIN);
    uint8_t dt  = digitalRead(ENC_DT_PIN);

    // Sadece CLK'nın düşen kenarını işle (gürültüyü azaltır)
    if (clk == LOW && lastCLK == HIGH) {
        if (dt == HIGH) {
            encoderTicks++;  // Saat yönü (CW)
        } else {
            encoderTicks--;  // Saat yönünün tersi (CCW)
        }
    }
    lastCLK = clk;
}

// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD_RATE);

    // Boot nedenini bridge.js'e bildir
    delay(200);
    Serial.print("@RST ");
    Serial.print((int)esp_reset_reason());
    Serial.print("\n");

    // HW-040 modülünün kendi pull-up'ları var → INPUT yeterli
    pinMode(ENC_CLK_PIN, INPUT);
    pinMode(ENC_DT_PIN,  INPUT);
    // SW için modülde pull-up olmayabilir → INPUT_PULLUP
    pinMode(ENC_SW_PIN,  INPUT_PULLUP);

    // İlk CLK durumunu kaydet (yanlış ilk tık önlemi)
    lastCLK = digitalRead(ENC_CLK_PIN);

    // CLK'ya interrupt bağla — CHANGE: her iki kenar da yakalanır
    attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), encoderISR, CHANGE);
}

// ─────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── 1. WATCHDOG (KALP ATIŞI) ─────────────────────────────
    if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
        Serial.write('?');  // Realtime → \n EKSİZ
        lastHeartbeatTime = now;
    }

    // ── 2. SW BUTON DEBOUNCE (Opsiyonel) ─────────────────────
    // İleride: eksen değiştirme, hız modu, JogCancel vb.
    int rawSW = digitalRead(ENC_SW_PIN);
    if (rawSW != lastButtonState) lastDebounceTime = now;
    if ((now - lastDebounceTime) > DEBOUNCE_DELAY_MS && rawSW != stableButtonState) {
        stableButtonState = rawSW;
        if (stableButtonState == LOW) {
            // Buton basıldı — ileride buraya aksiyon eklenecek
            // Örnek: Serial.write('\x85');  // JogCancel
        }
    }
    lastButtonState = rawSW;

    // ── 3. ENCODER TIK → $J= PAKETI ──────────────────────────
    if (now - lastPacketTime >= JOG_INTERVAL_MS) {
        lastPacketTime = now;

        // ISR'den atomik okuma: interrupt'ı kısa süre durdur
        noInterrupts();
        int32_t ticks = encoderTicks;
        encoderTicks  = 0;
        interrupts();

        if (ticks != 0) {
            float distance = ticks * JOG_STEP_MM;
            // Pozitif ticks = CW = X pozitif, negatif = CCW = X negatif
            String cmd = "$J=G91G21X" + String(distance, 2) + "F" + String(JOG_FEED_RATE) + "\n";
            Serial.print(cmd);
        }
    }

    // ── 4. GELİŞ BUFFER TEMİZLİĞİ ────────────────────────────
    while (Serial.available() > 0) {
        Serial.read();
    }
}

// ============================================================
//  Rownd CNC Handwheel — ESP32 USB-Serial Firmware
//  Hedef: ESP32-S3 (USB-CDC) → Raspberry Pi → bridge.js → cncjs
//
//  Bu sürüm: Tek buton ile sanal jog testi (encoder olmadan).
//  Buton basılı tutulduğu sürece X ekseni eksi yönde hareket eder.
// ============================================================
#include <Arduino.h>
#include "esp_system.h"  // esp_reset_reason()

// ── Donanım Sabitleri ────────────────────────────────────────
#define BUTTON_PIN          4       // Jog butonu (INPUT_PULLUP, GND'a çek)

// ── Protokol Sabitleri ───────────────────────────────────────
#define SERIAL_BAUD_RATE    115200  // bridge.js ile eşleşmeli

// ── Watchdog (Kalp Atışı) ────────────────────────────────────
// bridge.js, 600ms sessizlik → JogCancel (0x85) atar.
// Biz her 200ms'de bir '?' göndererek "hayattayım" deriz.
#define HEARTBEAT_INTERVAL_MS   200

// ── Accumulator (Paketleyici) Ayarları ──────────────────────
// Butondan gelen tıklar 150ms'de bir toplanıp tek $J= paketi olur.
#define JOG_INTERVAL_MS     150     // Paket yollama periyodu
#define JOG_STEP_MM         0.1f    // Tık başına mesafe (mm)
#define JOG_FEED_RATE       300     // mm/dak
#define TICK_RATE_MS        15      // Buton basılıyken kaç ms'de bir tık sayılsın

// ── Debounce ─────────────────────────────────────────────────
#define DEBOUNCE_DELAY_MS   50

// ── Durum Değişkenleri ───────────────────────────────────────
static unsigned long lastDebounceTime   = 0;
static int           lastButtonState    = HIGH;
static int           stableButtonState  = HIGH;

static int           accumulatedTicks   = 0;
static unsigned long lastTickTime       = 0;
static unsigned long lastPacketTime     = 0;
static unsigned long lastHeartbeatTime  = 0;

// ─────────────────────────────────────────────────────────────
void setup() {
    // USB-CDC üzerinden Raspberry Pi bridge.js'e bağlan
    Serial.begin(SERIAL_BAUD_RATE);

    // ESP32'nin bir önceki boot nedenini bridge.js'e bildir.
    // bridge.js @RST handler'ı bunu PENDANT RESET REASON: N olarak loglar,
    // sessiz çökmeler bu şekilde teşhis edilebilir.
    delay(100); // CDC bağlantısının oturması için kısa bekleme
    int reason = (int)esp_reset_reason();
    Serial.print("@RST ");
    Serial.print(reason);
    Serial.print("\n");

    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

// ─────────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // ── 1. WATCHDOG (KALP ATIŞI) ─────────────────────────────
    // '?' Grbl gerçek-zamanlı (realtime) komutudur: \n EKLENMEMELİ.
    // Serial.write() tek byte yollar, println() \r\n ekler — yanlış olur.
    if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
        Serial.write('?');
        lastHeartbeatTime = now;
    }

    // ── 2. BUTON DEBOUNCE ─────────────────────────────────────
    int rawButton = digitalRead(BUTTON_PIN);
    if (rawButton != lastButtonState) {
        lastDebounceTime = now;
    }
    if ((now - lastDebounceTime) > DEBOUNCE_DELAY_MS && rawButton != stableButtonState) {
        stableButtonState = rawButton;
    }
    lastButtonState = rawButton;

    // ── 3. TIK BİRİKTİRME ────────────────────────────────────
    // Buton basılıyken (LOW) her TICK_RATE_MS'de bir sayaç artır.
    // Gerçek bir rotary encoder olsaydı bu bölüm interrupt callback'i olurdu.
    if (stableButtonState == LOW) {
        if (now - lastTickTime >= TICK_RATE_MS) {
            accumulatedTicks++;
            lastTickTime = now;
        }
    }

    // ── 4. BİRİKEN TIK PAKET → $J= KOMUTU ───────────────────
    if (now - lastPacketTime >= JOG_INTERVAL_MS) {
        lastPacketTime = now;

        if (accumulatedTicks > 0) {
            float distance = accumulatedTicks * (-JOG_STEP_MM); // Eksi yön

            // $J= : Grbl'ın jogging komutu. G-Code programı sayılmaz,
            // cncjs sender'ın ok sayacını tüketmez. G91=artımlı, G21=mm.
            // Protokol: standart komut → sonuna \n OLMALI; println() \r\n ekler
            // ama bridge.js splitter \r'ı temizler; yine de temiz \n gönderelim.
            String cmd = "$J=G91G21X" + String(distance, 2) + "F" + String(JOG_FEED_RATE) + "\n";
            Serial.print(cmd);

            accumulatedTicks = 0;
        }
    }

    // ── 5. GELİŞ BUFFER TEMİZLİĞİ ────────────────────────────
    // bridge.js tarafından gönderilen "ok", "<Idle|MPos:...>" vb.
    // cevapları okuyarak Serial buffer'ın dolmasını engelle.
    // İleride ekrana yazdırmak için bu bölümü genişlet.
    while (Serial.available() > 0) {
        Serial.read(); // Şimdilik sadece boşalt
    }
}
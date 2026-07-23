#include <Arduino.h>
#include <Preferences.h> 
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include "config.h"
#include "ble_client.hpp"
#include <esp_gap_ble_api.h>

// --- ÇALIŞMA ZAMANI LOGLARI ŞALTERİ ---
// İşlemciyi gerçek zamanlı buton okurken yormamak için false kalmalıdır.
// Sadece gelen bildirimleri susturur, kurulum aşamaları gösterilmeye devam eder.
#define SHOW_RUNTIME_LOGS false 
// --- MAKİNE YANITLARINI DİNLEME (ÇİFT YÖNLÜ İLETİŞİM) ŞALTERİ ---
// true  = Makineden gelen ok ve konum (MPos) bildirimlerini dinler (Ekranlı modeller için).
// false = Sağır mod (Kör gönderim). Sadece komut yollar, dinlemeyi kapatarak hızı artırır.
#define ENABLE_NOTIFICATIONS false

// --- BUTON AYARLARI ---
const int BUTTON_PIN = 4;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 10;  // Daha hızlı tepki için düşük debounce
int lastButtonState = HIGH;
int stableButtonState = HIGH;

// Sürekli basım (Jog) ayarları
unsigned long lastJogTime = 0;
const unsigned long jogInterval = 25; // Saniyede ~40 komut
const float jogStepDistance = -0.02f;   // Küçük adım ile akıcı ve hassas hareket
const int jogFeedRate = 1000;

// --- HANDWHEEL SIMÜLASYONU (Biriktir ve Gönder) ---
static int accumulatedTicks = 0;          // Çevrilen ama henüz yollanmayan tık sayısı
static unsigned long lastSendTime = 0;
const unsigned long sendInterval = 1000;    // 50ms'de bir biriken tıkları paketle ve yolla
const float multiplier = 1;             // 1x ayarı: Her tık 0.1mm

BLEHandwheelClient bleClient;
static BLERemoteCharacteristic* pRemoteCharacteristicWrite = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristicNotify = nullptr;
static BLEClient* pClient = nullptr;

// NVS (Kalıcı hafıza) objesi ve MAC değişkeni
Preferences preferences; 
String savedMAC = "";
static String lastResponse = "";

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    // Sadece SHOW_RUNTIME_LOGS true ise burası işlemciyi meşgul eder
    if (SHOW_RUNTIME_LOGS) {
        String response = "";
        for (size_t i = 0; i < length; i++) {
            response += (char)pData[i];
        }
        // Satır sonundaki görünmez karakterleri (\r, \n) ve boşlukları temizleyelim
        response.trim(); 
        // Gelen mesaj boş değilse ve bir önceki mesajla aynı değilse ekrana yazdır
        if (response.length() > 0 && response != lastResponse) {
            Serial.print("Yanit: ");
            Serial.println(response);
            lastResponse = response;
        }
    }
}

// Sunucuya Bağlantı Kurma
bool connectToServer(BLEAdvertisedDevice* myDevice) {
    Serial.print("Sunucuya baglaniliyor: ");
    Serial.println(myDevice->getAddress().toString().c_str());

    pClient = BLEDevice::createClient();
    if (!pClient->connect(myDevice)) {
        Serial.println("Sunucuya baglanilamadi!");
        return false;
    }
    Serial.println("Sunucuya baglandi. BUTONA BASARAK HAREKET ETTIREBILIRSINIZ.");

    // --- HIZ AŞIRTMA: Connection Interval'i (Randevu Aralığı) minimuma çekiyoruz ---
    // DOĞRU VERİ TİPİ: esp_ble_gap_conn_params_t
    esp_ble_conn_update_params_t conn_params;
        memcpy(conn_params.bda, pClient->getPeerAddress().getNative(), sizeof(esp_bd_addr_t));
        conn_params.min_int = 0x06; // 7.5 milisaniye
        conn_params.max_int = 0x0C; // 15 milisaniye
        conn_params.latency = 0;
        conn_params.timeout = 400;  // 4 saniye
        
        esp_ble_gap_update_conn_params(&conn_params);
        // ------------------------------------------------------------------------------

    // BAĞLANTI BAŞARILIYSA: MAC ADRESİNİ HAFIZAYA KAYDET
    String connectedMAC = myDevice->getAddress().toString().c_str();
    if (savedMAC != connectedMAC) {
        preferences.putString("mac", connectedMAC);
        savedMAC = connectedMAC;
        Serial.println("Cihaz hafizaya kaydedildi. Gelecek sefer otomatik baglanilacak.");
    }

    BLERemoteService* pRemoteService = pClient->getService(BLEUUID(SERVICE_UUID.c_str()));
    if (pRemoteService == nullptr) {
        Serial.println("Servis bulunamadi!");
        pClient->disconnect();
        return false;
    }

    pRemoteCharacteristicWrite = pRemoteService->getCharacteristic(BLEUUID(CHARACTERISTIC_WRITE.c_str()));
    pRemoteCharacteristicNotify = pRemoteService->getCharacteristic(BLEUUID(CHARACTERISTIC_NOTIFY.c_str()));

    if (pRemoteCharacteristicWrite == nullptr) {
        Serial.println("Write karakteristigi bulunamadi!");
        pClient->disconnect();
        return false;
    }

    if (ENABLE_NOTIFICATIONS) {
        if (pRemoteCharacteristicNotify != nullptr && pRemoteCharacteristicNotify->canNotify()) {
            pRemoteCharacteristicNotify->registerForNotify(notifyCallback);
            Serial.println("Bildirimler acik. Makine dinleniyor...");
        }
    } else {
        Serial.println("Sagir Mod aktif. Makine bildirimleri goz ardi edilecek.");
    }

    return true;
}

// Özel komutları ve cihaz seçimini dinleyen birleşik fonksiyon
void checkSerialCommands() {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        
        if (input.length() == 0) return;

        if (input == "RESCAN") { 
            Serial.println("\n--- RESCAN KOMUTU ALINDI ---");
            Serial.println("Hafiza siliniyor, yeni cihaz aranacak...");
            
            // Flaş bellekten MAC'i sil
            preferences.remove("mac"); 
            savedMAC = "";
            bleClient.targetMAC = "";
            
            if (bleClient.connected && pClient != nullptr) {
                pClient->disconnect();
            }
            
            bleClient.connected = false;
            bleClient.startScan(); // Tekrar tarama başlat
        } 
        else if (bleClient.scanDone && !bleClient.connected && !bleClient.doConnect) {
            // Komut RESCAN değilse ve cihaz seçimi bekliyorsak:
            int selection = input.toInt(); 
            
            if (selection >= 0 && selection < (int)bleClient.foundDevices.size()) {
                bleClient.selectedDevice = new BLEAdvertisedDevice(bleClient.foundDevices[selection]);
                Serial.print("Secilen cihaz aliniyor: ");
                Serial.println(bleClient.selectedDevice->getAddress().toString().c_str());
                bleClient.doConnect = true;
            } else {
                Serial.println("Gecersiz secim! Tekrar deneyin.");
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Preferences alanını başlat ("rownd" adında bir sektör açıyoruz, false = Read/Write modu)
    preferences.begin("rownd", false);
    savedMAC = preferences.getString("mac", ""); // Kayıtlı veri yoksa "" (boş) döner
    
    delay(1000);
    
    if (savedMAC != "") {
        Serial.print("Hafizada kayitli cihaz var: ");
        Serial.println(savedMAC);
    } else {
        Serial.println("Hafizada cihaz yok. Yeni kurulum yapilacak.");
    }

    // Kayıtlı MAC adresiyle sistemi başlat
    BLEDevice::setMTU(512);
    bleClient.init(savedMAC);
}

void loop() {
    // Seri porttan gelen özel komutları (RESCAN) dinle
    checkSerialCommands();

    if (bleClient.doConnect) {
        if (bleClient.selectedDevice != nullptr && connectToServer(bleClient.selectedDevice)) {
            Serial.println("Baglanti isleme alindi!");
            bleClient.connected = true;
        } else {
            Serial.println("Baglanti basarisiz. Tekrar taraniyor...");
            bleClient.connected = false;
            bleClient.startScan();
        }
        bleClient.doConnect = false;
    }

    // --- STRESS TEST (SPAM) KODU ---
    if (bleClient.connected) {
        int currentButtonState = digitalRead(BUTTON_PIN);

        // Debounce: fiziksel düğme zıplamasını filtrele
        if (currentButtonState != lastButtonState) {
            lastDebounceTime = millis();
        }

        if ((millis() - lastDebounceTime) > debounceDelay && currentButtonState != stableButtonState) {
            stableButtonState = currentButtonState;

            // Bırakma anında yeni komut üretimini anında durdur
            if (stableButtonState == HIGH) {
                lastJogTime = 0;
            }
        }

        // Buton basılıyken el çarkı SOLA sürekli küçük adımlarla çevriliyormuş gibi yap
        if (stableButtonState == LOW && pRemoteCharacteristicWrite != nullptr) {
            if (lastJogTime == 0 || (millis() - lastJogTime) >= jogInterval) {
                String gcode = "G1 G91 X" + String(jogStepDistance, 2) + " F" + String(jogFeedRate);
                sendGCodeCommand(pRemoteCharacteristicWrite, gcode);
                lastJogTime = millis();
            }
        }

        lastButtonState = currentButtonState;
    }

    delay(1);
}




/*
    // --- STRESS TEST (SPAM) KODU ---
    if (bleClient.connected) {
        static unsigned long lastSpamTime = 0;
        const unsigned long spamInterval = 10; // 10ms = Saniyede 100 komut!

        if ((millis() - lastSpamTime) >= spamInterval) {
            // Zararsız komut: G21 (Milimetre Modu). Harekete sebep olmaz, sadece ok döndürür.
            sendGCodeCommand(pRemoteCharacteristicWrite, "G21");
            lastSpamTime = millis();
        }
    }
*/
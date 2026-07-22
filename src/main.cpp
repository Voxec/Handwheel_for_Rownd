#include <Arduino.h>
#include <Preferences.h> 
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include "config.h"
#include "ble_client.hpp"

// --- ÇALIŞMA ZAMANI LOGLARI ŞALTERİ ---
// İşlemciyi gerçek zamanlı buton okurken yormamak için false kalmalıdır.
// Sadece gelen bildirimleri susturur, kurulum aşamaları gösterilmeye devam eder.
#define SHOW_RUNTIME_LOGS true 

// --- BUTON AYARLARI ---
const int BUTTON_PIN = 4;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;  // İlk basım ark filtresi (hızlı tepki için 50ms'ye düşürdük)
int lastButtonState = HIGH;

// Sürekli basım (Jog) ayarları
unsigned long lastJogTime = 0;
const unsigned long jogInterval = 100; // Saniyede 5 komut için bekleme süresi (1000ms / 5 = 200ms)

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

    if (pRemoteCharacteristicNotify != nullptr && pRemoteCharacteristicNotify->canNotify()) {
        pRemoteCharacteristicNotify->registerForNotify(notifyCallback);
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

    // --- BUTON İLE KOMUT GÖNDERME (Akıcı Hareket ve Acil Duruş) ---
    if (bleClient.connected) {
        int currentButtonState = digitalRead(BUTTON_PIN);

        // Durum 1: Butona YENİ basıldı
        if (currentButtonState == LOW && lastButtonState == HIGH) {
            if ((millis() - lastDebounceTime) > debounceDelay) {
                sendGCodeCommand(pRemoteCharacteristicWrite, "G21 G91 X-2 F700"); // Mesafeyi bol verdik
                lastJogTime = millis();      
                lastDebounceTime = millis(); 
            }
        } 
        // Durum 2: Butona BASILI TUTULUYOR (100ms aralıkla)
        else if (currentButtonState == LOW && lastButtonState == LOW) {
            if ((millis() - lastJogTime) >= 100) {
                sendGCodeCommand(pRemoteCharacteristicWrite, "G21 G91 X-2 F700");
                lastJogTime = millis();
            }
        }
        // Durum 3: Butondan EL ÇEKİLDİ (Anında durdur!)
        else if (currentButtonState == HIGH && lastButtonState == LOW) {
            // GRBL için anında durdurma (Feed Hold) komutu
            // Rownd sistemi bunu direkt GRBL'ye iletiyorsa makine zınk diye durur.
            sendGCodeCommand(pRemoteCharacteristicWrite, "!"); 
        }
        
        lastButtonState = currentButtonState;
    }

    delay(1);
}
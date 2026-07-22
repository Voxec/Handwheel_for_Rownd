#include <Arduino.h>
#include <Preferences.h> // ESP32 Flaş Bellek Kütüphanesi
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include "config.h"
#include "ble_client.hpp"

// --- BUTON AYARLARI ---
const int BUTTON_PIN = 4;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 150;
int lastButtonState = HIGH;

BLEHandwheelClient bleClient;
static BLERemoteCharacteristic* pRemoteCharacteristicWrite = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristicNotify = nullptr;
static BLEClient* pClient = nullptr;

// NVS (Kalıcı hafıza) objesi ve MAC değişkeni
Preferences preferences; 
String savedMAC = "";
static String lastResponse = "";

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    String response = "";
    for (size_t i = 0; i < length; i++) {
        response += (char)pData[i];
    }
    
    // Satır sonundaki görünmez karakterleri (\r, \n) ve boşlukları temizleyelim
    response.trim(); 

    // Gelen mesaj boş değilse ve bir önceki mesajla aynı değilse ekrana yazdır
    if (response.length() > 0 && response != lastResponse) {
        DEBUG_PRINT("Yanit: ");
        DEBUG_PRINTLN(response);
        lastResponse = response;
    }
}

// Sunucuya Bağlantı Kurma
bool connectToServer(BLEAdvertisedDevice* myDevice) {
    DEBUG_PRINT("Sunucuya baglaniliyor: ");
    DEBUG_PRINTLN(myDevice->getAddress().toString().c_str());

    pClient = BLEDevice::createClient();
    if (!pClient->connect(myDevice)) {
        DEBUG_PRINTLN("Sunucuya baglanilamadi!");
        return false;
    }
    DEBUG_PRINTLN("Sunucuya baglandi. BUTONA BASARAK HAREKET ETTIREBILIRSINIZ.");

    // BAĞLANTI BAŞARILIYSA: MAC ADRESİNİ HAFIZAYA KAYDET
    String connectedMAC = myDevice->getAddress().toString().c_str();
    if (savedMAC != connectedMAC) {
        preferences.putString("mac", connectedMAC);
        savedMAC = connectedMAC;
        DEBUG_PRINTLN("Cihaz hafizaya kaydedildi. Gelecek sefer otomatik baglanilacak.");
    }

    BLERemoteService* pRemoteService = pClient->getService(BLEUUID(SERVICE_UUID.c_str()));
    if (pRemoteService == nullptr) {
        DEBUG_PRINTLN("Servis bulunamadi!");
        pClient->disconnect();
        return false;
    }

    pRemoteCharacteristicWrite = pRemoteService->getCharacteristic(BLEUUID(CHARACTERISTIC_WRITE.c_str()));
    pRemoteCharacteristicNotify = pRemoteService->getCharacteristic(BLEUUID(CHARACTERISTIC_NOTIFY.c_str()));

    if (pRemoteCharacteristicWrite == nullptr) {
        DEBUG_PRINTLN("Write karakteristigi bulunamadi!");
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
            DEBUG_PRINTLN("\n--- RESCAN KOMUTU ALINDI ---");
            DEBUG_PRINTLN("Hafiza siliniyor, yeni cihaz aranacak...");
            
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
                DEBUG_PRINT("Secilen cihaz aliniyor: ");
                DEBUG_PRINTLN(bleClient.selectedDevice->getAddress().toString().c_str());
                bleClient.doConnect = true;
            } else {
                DEBUG_PRINTLN("Gecersiz secim! Tekrar deneyin.");
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
        DEBUG_PRINT("Hafizada kayitli cihaz var: ");
        DEBUG_PRINTLN(savedMAC);
    } else {
        DEBUG_PRINTLN("Hafizada cihaz yok. Yeni kurulum yapilacak.");
    }

    // Kayıtlı MAC adresiyle sistemi başlat
    bleClient.init(savedMAC);
}

void loop() {
    // Seri porttan gelen özel komutları (RESCAN) dinle
    checkSerialCommands();


    if (bleClient.doConnect) {
        if (bleClient.selectedDevice != nullptr && connectToServer(bleClient.selectedDevice)) {
            DEBUG_PRINTLN("Baglanti isleme alindi!");
            bleClient.connected = true;
        } else {
            DEBUG_PRINTLN("Baglanti basarisiz. Tekrar taraniyor...");
            bleClient.connected = false;
            bleClient.startScan();
        }
        bleClient.doConnect = false;
    }

    // --- BUTON İLE KOMUT GÖNDERME ---
    if (bleClient.connected) {
        int currentButtonState = digitalRead(BUTTON_PIN);

        if (currentButtonState == LOW && lastButtonState == HIGH) {
            if ((millis() - lastDebounceTime) > debounceDelay) {
                sendGCodeCommand(pRemoteCharacteristicWrite, "G21 G91 X-1 F700");
                lastDebounceTime = millis();
            }
        }
        lastButtonState = currentButtonState;
    }

    delay(1);
}
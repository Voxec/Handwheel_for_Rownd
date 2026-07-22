#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include "config.h"
#include "ble_client.hpp"

// --- BUTON AYARLARI ---
const int BUTTON_PIN = 4; // Butonu GPIO 4 ve GND arasına bağla
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 150; // 150ms ark filtresi
int lastButtonState = HIGH;

BLEHandwheelClient bleClient;
static BLERemoteCharacteristic* pRemoteCharacteristicWrite = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristicNotify = nullptr;
static BLEClient* pClient = nullptr;

// Bildirim Geri Çağırımı (Sadece önemli yanıtları yazdıracak şekilde sadeleştirdik)
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
        
        Serial.print("Yanit: ");
        Serial.println(response);
        lastResponse = response;
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

void setup() {
    Serial.begin(115200);
    
    // Butonu dahili dirençle kur
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    delay(1000);
    bleClient.init();
}

void loop() {
    // Bağlantı kurulana kadar seri porttan cihaz seçimi yapılır
    bleClient.handleSerialSelection();

    if (bleClient.doConnect) {
        if (bleClient.selectedDevice != nullptr && connectToServer(bleClient.selectedDevice)) {
            Serial.println("Baglanti basarili!");
            bleClient.connected = true;
        } else {
            Serial.println("Baglanti basarisiz. Tekrar taranıyor...");
            bleClient.connected = false;
            bleClient.startScan();
        }
        bleClient.doConnect = false;
    }

    // --- BUTON İLE KOMUT GÖNDERME ---
    if (bleClient.connected) {
        int currentButtonState = digitalRead(BUTTON_PIN);

        // Butona basıldığında (Pull-up olduğu için LOW)
        if (currentButtonState == LOW && lastButtonState == HIGH) {
            if ((millis() - lastDebounceTime) > debounceDelay) {
                
                // Anında komutu yolla
                sendGCodeCommand(pRemoteCharacteristicWrite, "G21 G91 X-1 F700");
                
                lastDebounceTime = millis();
            }
        }
        lastButtonState = currentButtonState;
    }

    delay(1); // İşlemciyi kitlememek için minimal bekleme
}
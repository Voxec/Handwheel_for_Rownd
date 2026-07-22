#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include "config.h"
#include "ble_client.hpp"

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
        
        // EĞER MAKİNE HAREKET EDERKEN EKRAN YİNE DONARSA:
        // GRBL sürekli "<Idle|MPos..." veya "[GC:..." gibi durum raporları atar.
        // Konum sürekli değiştiği için "tekrar eden" mesaj filtresinden kaçıp ekranı kilitleyebilir.
        // Eğer böyle bir sorun yaşarsan aşağıdaki iki satırın başındaki "//" işaretlerini kaldırıp
        // bu durum bildirimlerini tamamen susturabilirsin:
        
        // if (response.startsWith("<") || response.startsWith("[")) return;

        Serial.print("Yanit: ");
        Serial.println(response);
        
        // Yeni mesajı hafızaya al
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
    Serial.println("Sunucuya baglandi.");

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
    delay(1000);
    bleClient.init();
}

void loop() {
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

    // Komut gönderme kısmı - Seri portu şişirmeyecek şekilde optimize edildi
    if (bleClient.connected && Serial.available() > 0) {
        String inputLine = Serial.readStringUntil('\n');
        inputLine.trim();
        if (inputLine.length() > 0) {
            sendGCodeCommand(pRemoteCharacteristicWrite, inputLine);
            delay(50); // İşlemcinin nefes alması için kısa bekleme
        }
    }

    delay(10);
}
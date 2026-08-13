// ============================================================
//  DEPRECATED — BLE MİMARİSİ (KULLANILMIYOR)
//
//  Bu dosya USB-Serial mimarisine geçişten önce kullanılıyordu.
//  main.cpp artık bu dosyayı include ETMİYOR.
//  Git tarihçesi için silinmedi; referans olarak bırakıldı.
//  Yeni mimari: ESP32 → USB-CDC → Raspberry Pi → bridge.js → cncjs
// ============================================================
#ifndef BLE_CLIENT_HPP
#define BLE_CLIENT_HPP

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>
#include "config.h"

// --- SERİ PORT ŞALTERİ (DEBUG MODU) ---
// Sistemi tamamen bağımsız hale getirmek için burayı false yapın.
#define DEBUG_MODE true

#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

class BLEHandwheelClient {
public:
    bool doConnect = false;
    bool connected = false;
    bool scanDone = false;
    String targetMAC = ""; // Hafızadan gelecek olan hedef MAC adresi
    BLEAdvertisedDevice* selectedDevice = nullptr;
    std::vector<BLEAdvertisedDevice> foundDevices;

    class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
        BLEHandwheelClient* parent;
    public:
        MyAdvertisedDeviceCallbacks(BLEHandwheelClient* p) : parent(p) {}
        
        void onResult(BLEAdvertisedDevice advertisedDevice) {
            // Sadece Rownd servis UUID'sini yayan cihazları listeye alıyoruz
            if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID.c_str()))) {
                String currentMAC = advertisedDevice.getAddress().toString().c_str();

                // EĞER HAFIZADA KAYITLI BİR CİHAZ VARSA VE BUNA DENK GELDİYSEK:
                if (parent->targetMAC.length() > 0 && currentMAC == parent->targetMAC) {
                    DEBUG_PRINTLN("\n[OTOMATIK] Kayitli cihaz bulundu, hemen baglaniliyor...");
                    parent->selectedDevice = new BLEAdvertisedDevice(advertisedDevice);
                    parent->doConnect = true;
                    BLEDevice::getScan()->stop(); // Taramayı anında kes
                    return; 
                }

                // Kayıtlı cihaz aranmıyorsa (veya bulunamadıysa) standart listelemeyi yap
                bool exists = false;
                for (auto& d : parent->foundDevices) {
                    if (d.getAddress().equals(advertisedDevice.getAddress())) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    parent->foundDevices.push_back(advertisedDevice);
                    DEBUG_PRINT("[BULUNDU] MAC: ");
                    DEBUG_PRINT(currentMAC);
                    if (advertisedDevice.haveName()) {
                        DEBUG_PRINT(" | Isim: ");
                        DEBUG_PRINT(advertisedDevice.getName().c_str());
                    }
                    DEBUG_PRINTLN("");
                }
            }
        }
    };

    // Init fonksiyonu artık hafızadaki MAC'i parametre alıyor
    void init(String savedMAC) {
        targetMAC = savedMAC;
        DEBUG_PRINTLN("Sanal Handwheel Baslatiliyor...");
        BLEDevice::init("");
        startScan();
    }

    void startScan() {
        foundDevices.clear();
        scanDone = false;
        selectedDevice = nullptr;
        doConnect = false;
        
        BLEScan* pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(this));
        pBLEScan->setActiveScan(true);
        DEBUG_PRINTLN("Cevre taraniyor (5 saniye)...");
        pBLEScan->start(5, false);
        
        // Eğer otomatik bağlanma tetiklenmediyse (doConnect hala false ise) listeyi yazdır
        if (!doConnect) {
            printDeviceList();
        }
    }

    void printDeviceList() {
        DEBUG_PRINTLN("\n--- BULUNAN ROWND CIHAZLARI ---");
        if (foundDevices.empty()) {
            DEBUG_PRINTLN("Hicbir cihaz bulunamadi. Tekrar taraniyor...");
            startScan();
            return;
        }

        for (size_t i = 0; i < foundDevices.size(); i++) {
            DEBUG_PRINT("[");
            DEBUG_PRINT(i);
            DEBUG_PRINT("] MAC: ");
            DEBUG_PRINT(foundDevices[i].getAddress().toString().c_str());
            DEBUG_PRINTLN("");
        }
        DEBUG_PRINTLN("Baglanmak istediginiz cihazin numarasini (0, 1, ...) gonderin:");
        scanDone = true;
    }
};

void sendGCodeCommand(BLERemoteCharacteristic* pRemoteCharacteristic, String gcodeCommand) {
    if (pRemoteCharacteristic == nullptr) {
        return;
    }

    // 1. Önce beklenen başlık (header) kısmını oluşturuyoruz
    String header = "{\"type\":\"command\"}\n";
    
    // 2. Asıl komut kısmını (payload) JSON içine yerleştiriyoruz
    String payload = "{\"command\":{\"name\":\"gcode\",\"args\":\"" + gcodeCommand + "\"}}";
    
    // 3. İkisini birleştiriyoruz
    String finalMessage = header + payload;
    
    // 4. Hazırlanan doğru formatı BLE üzerinden yolluyoruz
    if (pRemoteCharacteristic->canWriteNoResponse()) {
        pRemoteCharacteristic->writeValue((uint8_t*)finalMessage.c_str(), finalMessage.length(), false);
    } else {
        pRemoteCharacteristic->writeValue(finalMessage.c_str(), finalMessage.length());
    }
    
    // HIZ İÇİN KAPATILDI:
    // Serial.print("Gonderildi: ");
    // Serial.println(finalMessage);
}

#endif
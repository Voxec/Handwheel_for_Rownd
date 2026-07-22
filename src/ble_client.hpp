#ifndef BLE_CLIENT_HPP
#define BLE_CLIENT_HPP

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <vector>
#include "config.h"

// Ön bildirimler
bool connectToServer(BLEAdvertisedDevice* myDevice);

class BLEHandwheelClient {
public:
    bool doConnect = false;
    bool connected = false;
    bool scanDone = false;
    BLEAdvertisedDevice* selectedDevice = nullptr;
    std::vector<BLEAdvertisedDevice> foundDevices;

    class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
        BLEHandwheelClient* parent;
    public:
        MyAdvertisedDeviceCallbacks(BLEHandwheelClient* p) : parent(p) {}
        
        void onResult(BLEAdvertisedDevice advertisedDevice) {
            // Sadece Rownd servis UUID'sini yayan cihazları listeye alıyoruz
            if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID.c_str()))) {
                bool exists = false;
                for (auto& d : parent->foundDevices) {
                    if (d.getAddress().equals(advertisedDevice.getAddress())) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    parent->foundDevices.push_back(advertisedDevice);
                    Serial.print("[BULUNDU] MAC: ");
                    Serial.print(advertisedDevice.getAddress().toString().c_str());
                    if (advertisedDevice.haveName()) {
                        Serial.print(" | İsim: ");
                        Serial.print(advertisedDevice.getName().c_str());
                    }
                    Serial.println();
                }
            }
        }
    };

    void init() {
        Serial.println("Sanal Handwheel Baslatiliyor...");
        BLEDevice::init("");
        startScan();
    }

    void startScan() {
        foundDevices.clear();
        scanDone = false;
        selectedDevice = nullptr;
        
        BLEScan* pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(this));
        pBLEScan->setActiveScan(true);
        Serial.println("Cevre taraniyor (5 saniye)...");
        pBLEScan->start(5, false);
        
        printDeviceList();
    }

    void printDeviceList() {
        Serial.println("\n--- BULUNAN ROWND CIHAZLARI ---");
        if (foundDevices.empty()) {
            Serial.println("Hicbir cihaz bulunamadi. Tekrar taraniyor...");
            startScan();
            return;
        }

        for (size_t i = 0; i < foundDevices.size(); i++) {
            Serial.print("[");
            Serial.print(i);
            Serial.print("] MAC: ");
            Serial.print(foundDevices[i].getAddress().toString().c_str());
            if (foundDevices[i].haveName()) {
                Serial.print(" | İsim: ");
                Serial.print(foundDevices[i].getName().c_str());
            }
            Serial.println();
        }
        Serial.println("Baglanmak istedginiz cihazin numarasini (0, 1, ...) Serial Monitor'e yazip Enter'a basin:");
        scanDone = true;
    }

    void handleSerialSelection() {
        if (scanDone && !connected && !doConnect && Serial.available() > 0) {
            int selection = Serial.parseInt();
            // Satır sonu karakterlerini temizle
            while(Serial.available() > 0) Serial.read();

            if (selection >= 0 && selection < (int)foundDevices.size()) {
                selectedDevice = new BLEAdvertisedDevice(foundDevices[selection]);
                Serial.print("Secilen cihaz aliniyor: ");
                Serial.println(selectedDevice->getAddress().toString().c_str());
                doConnect = true;
            } else {
                Serial.println("Gecersiz secim! Tekrar deneyin.");
            }
        }
    }
};

// G-code komutunu Raspberry Pi'ye göndermek için yardımcı fonksiyon
void sendGCodeCommand(BLERemoteCharacteristic* pRemoteCharacteristic, String gcodeCommand) {
    // 1. Önce beklenen başlık (header) kısmını oluşturuyoruz
    String header = "{\"type\":\"command\"}\n";
    
    // 2. Asıl komut kısmını (payload) JSON içine yerleştiriyoruz
    String payload = "{\"command\":{\"name\":\"gcode\",\"args\":\"" + gcodeCommand + "\"}}";
    
    // 3. İkisini birleştiriyoruz
    String finalMessage = header + payload;
    
    // 4. Hazırlanan doğru formatı BLE üzerinden yolluyoruz
    pRemoteCharacteristic->writeValue(finalMessage.c_str(), finalMessage.length());
    
    // HIZ İÇİN KAPATILDI:
    // Serial.print("Gonderildi: ");
    // Serial.println(finalMessage);
}

#endif
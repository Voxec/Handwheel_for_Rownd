#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Bağlanmak istediğimiz Raspberry Pi'nin Bluetooth adı
const String TARGET_DEVICE_NAME = "Rownd-39e343a45497c770"; 

// Rownd Raspberry Pi BLE UUID Değerleri
const String SERVICE_UUID           = "27cf08c1-076a-41af-becd-02ed6f6109b9";
const String CHARACTERISTIC_READ    = "a81d2e68-6b9a-4d24-bf0e-ee829f09b311";
const String CHARACTERISTIC_WRITE   = "ed50d118-53fd-4dc2-88b0-3c07f5a2a89a";
const String CHARACTERISTIC_NOTIFY  = "de2f08e3-f66c-4825-91e8-ace63bafed3d";

#endif
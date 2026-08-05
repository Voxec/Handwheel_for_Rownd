# ESP32 USB CNC Handwheel — USB Seri Migrasyonu & Sistem Sağlamlaştırma

## Genel Bakış

BLE'den USB Seri'ye geçiş yapılan projenin mevcut kod tabanı incelendi. `main.cpp` büyük ölçüde doğru mimariye getirilmiş; ancak birkaç kritik protokol hatası, gereksiz BLE artıkları ve ek sağlamlık iyileştirmeleri gerekiyor. `bridge.js` ise tamamen hazır ve production-grade durumda.

---

## User Review Required

> [!IMPORTANT]
> `main.cpp`'deki `Serial.println()` çağrısı `\r\n` gönderiyor. `bridge.js`'nin `latin1` tabanlı stream parser'ı bunu kabul ediyor ancak **protokol kurallarınız** `\n` ile bitiş şartını belirtmektedir. Arduino'da salt `\n` göndermek için `Serial.print(gcode + "\n")` kullanılmalıdır. Bu düzeltme geriye dönük uyumlu; `bridge.js` zaten `\r` karakterini splitter'da (`ch !== '\r'`) görmezden geliyor.

> [!WARNING]
> `ble_client.hpp` ve `config.h` BLE UUID/MAC bilgilerini içeriyor ve `main.cpp` tarafından artık `#include` edilmiyor. Bu dosyalar **silinmemeli** (git tarihçesi için) ama aktif derlemenin dışındalar. `config.h` içindeki BLE sabitlerini USB için yeni sabitlere dönüştüreceğiz.

> [!CAUTION]
> **`main.cpp` satır 80**: `G1 G91 X...` komutu **$J= (Jog) komutu değil, normal G-Code hareketi**. Grbl/FluidNC'de `G1` komutu iş programının bir parçası sayılır ve cncjs sender'ın `ok` sayacından birini tüketir. Doğru jogging için `$J=G91G21X-0.50F300\n` formatı kullanılmalıdır. Bu kritik bir düzeltmedir.

---

## Open Questions

> [!IMPORTANT]
> **Soru 1 — Gerçek encoder var mı?** `main.cpp`'de şu an buton ile simüle edilmiş "15ms'de bir tık" mantığı var. Projede bir **rotary encoder** (A/B fazlı) var mı, yoksa buton tabanlı simülasyon mu devam etmeli?

> [!IMPORTANT]  
> **Soru 2 — Ekran/Display var mı?** `ble_client.hpp` dosyası bir M5Dial/M5Stack bileşeni için yazılmış görünüyor (bridge.js log'larında `m5dial` geçiyor). ESP32 kartında bir display var mı? Varsa hangi kütüphaneyi kullanmak istiyorsunuz?

> [!IMPORTANT]
> **Soru 3 — `@RST` boot mesajı istiyor musunuz?** bridge.js `@RST <n>` komutuyla ESP32'nin önceki boot nedenini logluyor. Bunu `setup()` içinde göndermek ister misiniz?

---

## Proposed Changes

### ESP32 Firmware (`src/`)

#### [MODIFY] [main.cpp](file:///c:/Users/saris/Desktop/Masaüstü/Coding/Rownd/Handwheel_s3/src/main.cpp)

**Düzeltme 1 — `G1` → `$J=` (Kritik):**
```diff
- String gcode = "G1 G91 X" + String(totalDistance, 2) + " F" + String(jogFeedRate);
- Serial.println(gcode);
+ String gcode = "$J=G91G21X" + String(totalDistance, 2) + "F" + String(jogFeedRate) + "\n";
+ Serial.print(gcode);
```

**Düzeltme 2 — `\r\n` → `\n` (Protokol temizliği):**
`Serial.println()` yerine `Serial.print(str + "\n")` kullanımına geçiş.

**Ekleme 3 — `@RST` Boot Bildirimi:**
```cpp
void setup() {
    Serial.begin(115200);
    // Boot nedenini bridge'e bildir (bridge.js @RST handler'ı logluyor)
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.print("@RST ");
    Serial.print((int)reason);
    Serial.print("\n");
    ...
}
```

**Ekleme 4 — Gerçek Encoder Desteği (Opsiyonel, Soru 1'e bağlı):**
A/B fazlı rotary encoder için interrupt tabanlı sayaç. Mevcut buton simülasyonu encoder yoksa korunacak.

#### [MODIFY] [config.h](file:///c:/Users/saris/Desktop/Masaüstü/Coding/Rownd/Handwheel_s3/src/config.h)

BLE sabitleri yerine USB Seri konfigürasyon sabitleri:
```cpp
// Baud rate (bridge.js ile eşleşmeli)
#define SERIAL_BAUD_RATE  115200

// Watchdog kalp atışı aralığı (bridge.js PENDANT_SILENCE_MS=600'dan küçük olmalı)
#define HEARTBEAT_INTERVAL_MS  200

// Accumulator paket yollama aralığı
#define JOG_INTERVAL_MS  150

// Encoder/buton başına mesafe (mm)
#define JOG_STEP_MM  0.1f

// Jog besleme hızı (mm/dak)
#define JOG_FEED_RATE  300
```

#### [MODIFY] [ble_client.hpp](file:///c:/Users/saris/Desktop/Masaüstü/Coding/Rownd/Handwheel_s3/src/ble_client.hpp)

BLE header'ı `usb_serial.hpp` olarak yeniden adlandırılmayacak (git tarihçesi bozulur). Dosya başına büyük bir `// DEPRECATED` yorumu eklenerek artık kullanılmadığı belirtilecek. `main.cpp` zaten bu dosyayı include etmiyor.

---

### Node.js Bridge (`bridge.js`)

`bridge.js` **tamamen hazır ve değişiklik gerektirmiyor**. Mevcut haliyle:
- ✅ XON (0x11) filtrelemesi yapıyor
- ✅ Hang watchdog (600ms sessizlik → 0x85 JogCancel) çalışıyor
- ✅ `@RST`, `@LOAD`, `@START`, `@STOP`, `@PAUSE`, `@LIST` komutları işleniyor
- ✅ Otomatik serial port keşfi ve yeniden bağlanma var
- ✅ Workflow lockout (program çalışırken jog kilidi) aktif

---

## Verification Plan

### Automated Tests
- PlatformIO derleme: `pio run -e esp32-s3-devkitc-1-n16r8v` (derleme hatası olmamalı)

### Manual Verification
1. **`$J=` vs `G1`**: Flash sonrası Serial Monitor'da gelen paketlerin `$J=G91G21X...` formatında olduğunu doğrulayın
2. **Watchdog testi**: bridge.js `--passthrough` modunda çalıştırın, her ~200ms'de `3f` (hex için `?`) gördüğünüzü doğrulayın
3. **XON testi**: ESP32'nin `0x11` göndermediğini doğrulayın (passthrought log'da `11` hex görünmemeli)
4. **`@RST` testi**: Bridge.js log'unda `PENDANT RESET REASON: 1 (POWERON)` benzeri bir satır görmelisiniz
5. **Jog cancel testi**: Pendant'ı jogging yaparken USB'yi çekin → bridge.js `pendant closed — sent JogCancel (0x85) fail-safe` loglamalı

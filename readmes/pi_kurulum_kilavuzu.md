# Pi Kurulum Kılavuzu — `bridge.js` (Raspberry Pi)

/opt/rownd-pendant-bridge   :   pi file locations
.service file should be transferred to /etc/systemd/system/ location 


journalctl -u rownd-pendant-bridge -f
ls -la /dev/rownd-pendant
lsusb
journalctl -u rownd-pendant-bridge -f | grep -E
sudo systemctl restart rownd-pendant-bridge

echo "=== SERVİS ===" && sudo systemctl is-active rownd-pendant-bridge && echo "=== PORT ===" && ls -la /dev/rownd-pendant 2>/dev/null || echo "PORT YOK" && echo "=== USB ===" && lsusb | grep 10c4

https://abamaelektronik.com.tr/urun/el-carki-gobegi-cap-60/


raspberry açıldıktan sonra, rownd app otomatik açılır, ama handwheelin algılanamsı için uygulamayı kapatıp tekrar açmak gerekiyor.



Bu kılavuz, ESP32 el çarkı ile cncjs arasındaki köprü yazılımını (`bridge.js`) Raspberry Pi'ye kurar ve systemd servisi olarak çalıştırır.

---

## Ön Koşullar

Pi'de aşağıdakilerin kurulu olması gerekiyor. Kontrol et:

```bash
node --version   # >= 18 olmalı
npm --version    # npm varsa node vardır
```

Eğer Node.js yoksa:
```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
```

---

## 1. Kurulum Dizini Oluştur

```bash
sudo mkdir -p /opt/rownd-pendant-bridge
sudo chown rownd:rownd /opt/rownd-pendant-bridge
```

---

## 2. Dosyaları Kopyala

`bridge/` klasöründeki iki dosyayı Pi'ye kopyala:
- `bridge.js`
- `package.json`

**Geliştirme makinenizden SCP ile:**
```bash
scp rownd-mods/pendant/bridge/bridge.js    rownd@<PI_IP>:/opt/rownd-pendant-bridge/
scp rownd-mods/pendant/bridge/package.json rownd@<PI_IP>:/opt/rownd-pendant-bridge/
```

**Veya Pi üzerinde doğrudan Git clone ile** (repo erişimin varsa):
```bash
# Pi'de:
cd /opt/rownd-pendant-bridge
# bridge.js ve package.json'u buraya al
```

---

## 3. npm Bağımlılıklarını Kur

Pi'de:
```bash
cd /opt/rownd-pendant-bridge
npm install
```

Bu komut `package.json`'daki bağımlılıkları indirir:
- `jsonwebtoken` — cncjs JWT kimlik doğrulaması
- `socket.io-client` — cncjs WebSocket bağlantısı
- `serialport` — ESP32 USB-CDC okuma/yazma

> **Not:** `serialport` native modül içerir, derleme için `build-essential` gerekebilir:
> ```bash
> sudo apt-get install -y build-essential python3
> ```

---

## 4. udev Kuralı — Sabit Port Adı

ESP32 her takıldığında `/dev/ttyACM0`, `/dev/ttyACM1` vb. değişebilir. Sabit `/dev/rownd-pendant` sembolik linki oluştur:

```bash
# ESP32'nin USB vendor/product ID'sini bul (bağlıyken):
lsusb
# Örnek çıktı: ID 303a:1001 Espressif Systems
```

```bash
sudo nano /etc/udev/rules.d/99-rownd-pendant.rules
```

Dosya içeriği (ESP32-S3 için):
```
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", SYMLINK+="rownd-pendant", MODE="0666"
```

> **idVendor/idProduct doğrulama:**
> ```bash
> udevadm info -a -n /dev/ttyACM0 | grep -E "idVendor|idProduct"
> ```

Kuralı uygula:
```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
# ESP32'yi çıkar/tak — /dev/rownd-pendant olduğunu doğrula:
ls -la /dev/rownd-pendant
```

---

## 5. systemd Servisi

Servis dosyasını kopyala:
```bash
sudo cp rownd-mods/pendant/bridge/rownd-pendant-bridge.service \
    /etc/systemd/system/rownd-pendant-bridge.service
```

Etkinleştir ve başlat:
```bash
sudo systemctl daemon-reload
sudo systemctl enable rownd-pendant-bridge
sudo systemctl start rownd-pendant-bridge
```

Durum kontrol:
```bash
sudo systemctl status rownd-pendant-bridge
```

Log izle:
```bash
journalctl -u rownd-pendant-bridge -f
```

---

## 6. Manuel Test (Servis Olmadan)

Servisi kurmadan önce doğru çalıştığını doğrulamak için:

**Stub modu** (ESP32 olmadan, klavyeden komut gir):
```bash
cd /opt/rownd-pendant-bridge
node bridge.js
# Prompt'a: ?    (cncjs'den yanıt gelmeli)
# Prompt'a: $G   (modal durumu sorgula)
```

**Passthrough modu** (ESP32 bağlıyken, cncjs olmadan — byte log):
```bash
node bridge.js --passthrough /dev/rownd-pendant
# Her ~200ms'de "3f" (?) hex görmeli → Watchdog çalışıyor
```

**Gerçek mod** (ESP32 + cncjs):
```bash
node bridge.js --pendant /dev/rownd-pendant
```

**Smoke test** (bağlantı var mı hızlı kontrol):
```bash
node bridge.js --smoke
# "smoke: PASS" görünmeli
```

---

## 7. İlk Bağlantı Doğrulaması

ESP32 bağlıyken bridge.js logunda şunları görmeli:

```
[...] pendant serial: opened /dev/rownd-pendant @ 115200
[...] *** PENDANT RESET REASON: 1 (POWERON) ***      ← @RST bildirimi
[...] connected sid=...                               ← cncjs bağlantısı
[...] active serial port: /dev/ttyUSB0               ← CNC makinesi
[...] pendant>cncjs 1B  hex=3f                       ← Watchdog '?' her 200ms
```

Butona basınca:
```
[...] pendant>cncjs ...B  hex=24 4a 3d 47 39 31 47 32 31 58 2d ... 0a
#                          $  J  =  G  9  1  G  2  1  X  -  ...  \n
```

---

## 8. Sorun Giderme

| Belirti | Neden | Çözüm |
|---|---|---|
| `/dev/rownd-pendant` yok | udev kuralı eşleşmedi | `lsusb` ile idVendor/idProduct doğrula |
| `EACCES` serial port | Kullanıcı `dialout` grubunda değil | `sudo usermod -aG dialout rownd` |
| `no responding cncjs` | Rownd uygulaması çalışmıyor | Rownd UI'ı başlat, `--smoke` ile test et |
| `error:1 (Expected command letter)` | 0x11 (XON) gönderiliyor | ESP32 kodunda `Serial.write(0x11)` varsa kaldır |
| Watchdog sessizlik uyarısı | ESP32 dondu | Pendant'ı çıkar/tak, firmware'i yeniden flash'la |

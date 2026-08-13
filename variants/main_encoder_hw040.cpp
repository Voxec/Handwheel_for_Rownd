#include <Arduino.h>

const int ROTARY_ENCODER_A_PIN = 26; // PinCLK
const int ROTARY_ENCODER_B_PIN = 27; // PinDT

volatile int encoderValue = 0;
int lastReportedValue = 0;

// --- KUSURSUZ DURUM MAKİNESİ (STATE MACHINE) ISR ---
void IRAM_ATTR handleEncoderChange() {
  static uint8_t old_AB = 0;
  
  // A ve B (CLK ve DT) pinlerini aynı anda oku
  uint8_t A = digitalRead(ROTARY_ENCODER_A_PIN);
  uint8_t B = digitalRead(ROTARY_ENCODER_B_PIN);
  
  // Şimdiki durumu 2 bitlik bir sayıya çevir (Örn: A=1, B=0 ise 10 olur)
  uint8_t current_AB = (A << 1) | B;
  
  // Önceki durum (old_AB) ile şimdiki durumu (current_AB) birleştirip 4 bitlik bir yön haritası çıkar
  uint8_t state = (old_AB << 2) | current_AB;

  // Saat yönü (CW) için sadece fiziksel olarak mümkün olan geçişleri kabul et
  if (state == 0b1101 || state == 0b0100 || state == 0b0010 || state == 0b1011) {
    encoderValue++;
  }
  // Saat yönünün tersi (CCW) için sadece geçerli geçişleri kabul et
  else if (state == 0b1110 || state == 0b0111 || state == 0b0001 || state == 0b1000) {
    encoderValue--;
  }
  
  // Bir sonraki kesme (interrupt) için şimdiki durumu kaydet
  old_AB = current_AB;
}

void setup() {
  Serial.begin(115200);

  pinMode(ROTARY_ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENCODER_B_PIN, INPUT_PULLUP);

  // DİKKAT: Artık FALLING değil, her iki pinde de CHANGE kullanıyoruz!
  attachInterrupt(digitalPinToInterrupt(ROTARY_ENCODER_A_PIN), handleEncoderChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_ENCODER_B_PIN), handleEncoderChange, CHANGE);
  
  Serial.println("State Machine Testi Basladi...");
}

void loop() {
  noInterrupts();
  int currentValue = encoderValue / 2;
  interrupts();

  if (lastReportedValue != currentValue) {
    // NOT: Enkoderin iç yapısına (full-step) göre her fiziksel tıkta sayı 2 artabilir.
    // Şimdilik ham değeri yazdırıyoruz, eğer her tıkta 2 artarsa currentValue / 2 yapacağız.
    Serial.println(currentValue);
    lastReportedValue = currentValue;
  }
  
  delay(10);
}
/*
  Bike Speed — versão USB (protótipo)
  Arduino Uno / Nano / Mega ligado por cabo no computador. Sem Bluetooth, sem Wi-Fi.

  Ligação:  sensor reed entre o pino 2 e o GND (sem resistor, usa o pull-up interno).
  Abra o app (Chrome/Edge no computador) > engrenagem > "Conectar por cabo USB".
  IMPORTANTE: feche o Monitor Serial do Arduino IDE, senão a porta fica ocupada.

  Protocolo (texto, 115200 baud, uma linha por mensagem):
    Arduino -> app, 5x por segundo:
      T,vel*10,media*10,max*10,metros,bateria,flags
      flags: bit0 = em movimento, bit1 = percurso em andamento
    app -> Arduino:
      C,1,b0,b1,b2,b3   iniciar percurso (meta em metros, 32 bits little-endian)
      C,2                encerrar percurso
      C,3,b0,b1         circunferência da roda em mm (16 bits little-endian)
*/
#include <EEPROM.h>

const uint8_t  PIN_REED = 2;            // pino de interrupção (Uno/Nano: 2 ou 3)
const uint16_t WHEEL_MM_DEFAULT = 2100;
const uint16_t DEBOUNCE_MS = 15;
const uint16_t STOP_MS = 3000;          // sem pulso por 3 s = parado

volatile uint32_t lastPulseMs = 0, intervalMs = 0, pulses = 0;
uint16_t wheelMm = WHEEL_MM_DEFAULT;

bool     running = false;
uint32_t rideStartPulses = 0;
float    maxKmh = 0, movingS = 0;
uint32_t lastTick = 0;
char     line[40];
uint8_t  n = 0;

void onPulse() {
  uint32_t now = millis();
  uint32_t dt = now - lastPulseMs;
  if (dt < DEBOUNCE_MS) return;
  intervalMs = dt;
  lastPulseMs = now;
  pulses++;
}

void loadWheel() {
  uint16_t w = EEPROM.read(0) | (EEPROM.read(1) << 8);
  wheelMm = (w >= 1000 && w <= 3000) ? w : WHEEL_MM_DEFAULT;
}

void handleCommand(char *s) {            // s = "C,1,16,39,0,0"
  uint8_t b[8]; uint8_t k = 0;
  char *p = s + 2;
  while (*p && k < 8) { b[k++] = (uint8_t)atoi(p); while (*p && *p != ',') p++; if (*p) p++; }
  if (k >= 1 && b[0] == 1 && k >= 5) {   // iniciar
    noInterrupts(); rideStartPulses = pulses; interrupts();
    maxKmh = 0; movingS = 0; running = true;
  } else if (k >= 1 && b[0] == 2) {      // encerrar
    running = false;
  } else if (k >= 3 && b[0] == 3) {      // roda
    uint16_t w = b[1] | (b[2] << 8);
    if (w >= 1000 && w <= 3000) {
      wheelMm = w;
      EEPROM.update(0, w & 0xFF); EEPROM.update(1, w >> 8);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_REED, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_REED), onPulse, FALLING);
  loadWheel();
  lastTick = millis();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n >= 3 && line[0] == 'C' && line[1] == ',') handleCommand(line);
      n = 0;
    } else if (n < sizeof(line) - 1) line[n++] = c;
  }

  uint32_t now = millis();
  if (now - lastTick < 200) return;
  float dtS = (now - lastTick) / 1000.0f;
  lastTick = now;

  noInterrupts();
  uint32_t last = lastPulseMs, itv = intervalMs, p = pulses;
  interrupts();

  bool moving = itv > 0 && (now - last) < STOP_MS;
  float kmh = 0;
  if (moving) {
    float s = (wheelMm / 1000.0f) / (itv / 1000.0f);   // m/s
    kmh = s * 3.6f;
    // se o último pulso já está atrasado, a velocidade real é menor
    float since = (now - last) / 1000.0f;
    if (since > itv / 1000.0f) kmh = (wheelMm / 1000.0f) / since * 3.6f;
  }

  float distM = 0;
  if (running) {
    distM = (p - rideStartPulses) * (wheelMm / 1000.0f);
    if (moving) { movingS += dtS; if (kmh > maxKmh) maxKmh = kmh; }
  }
  float avg = (movingS > 0) ? (distM / movingS) * 3.6f : 0;

  Serial.print(F("T,"));
  Serial.print((uint16_t)(kmh * 10)); Serial.print(',');
  Serial.print((uint16_t)(avg * 10)); Serial.print(',');
  Serial.print((uint16_t)(maxKmh * 10)); Serial.print(',');
  Serial.print((uint32_t)distM); Serial.print(',');
  Serial.print(100); Serial.print(',');
  Serial.println((moving ? 1 : 0) | (running ? 2 : 0));
}

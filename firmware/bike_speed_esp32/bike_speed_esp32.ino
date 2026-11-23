/*
  Bike Speed — firmware do ESP32 (Arduino IDE)

  O que faz:
    - Lê o sensor magnético (reed) da roda e calcula velocidade atual, média,
      máxima e distância do percurso.
    - Fala por Bluetooth (BLE) com o app "Bike Speed" (PWA).
    - Guarda a circunferência da roda na memória do ESP32.

  Ligação:
    - Sensor do velocímetro (reed, 2 fios, sem polaridade):
        um fio no GND e o outro no GPIO 27.
        Capacitor de 100 nF entre os dois fios é opcional (filtra ruído).
    - Bateria (opcional, veja USE_BATTERY_ADC abaixo).

  Teste sem sensor: encoste um fio do GPIO 27 no GND várias vezes. Cada toque conta como uma volta.

  Arduino IDE:
    - Placa: "ESP32 Dev Module" (pacote "esp32" da Espressif, versão 2.x ou 3.x).
    - Não precisa instalar bibliotecas: BLE e Preferences já vêm no pacote.
    - A pasta do sketch precisa se chamar bike_speed_esp32.

  Protocolo (igual ao do app, veja o README):
    Nome: BikeSpeed
    Serviço:    b1cec10c-0001-4e5a-9a3b-5eb1c0de0001
    Telemetria: b1cec10c-0002-...  (NOTIFY, 12 bytes, little-endian)
      [0-1] velocidade atual (km/h x 10)   [2-3] média (km/h x 10)   [4-5] máxima (km/h x 10)
      [6-9] distância do percurso (metros) [10] bateria (%)
      [11]  flags: bit0 = em movimento, bit1 = percurso em andamento
    Comando:    b1cec10c-0003-...  (WRITE)
      0x01 + uint32 objetivo (m)  iniciar percurso (zera distância, média e máxima)
      0x02                        finalizar percurso
      0x03 + uint16 roda (mm)     circunferência da roda
*/

#include <Arduino.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
#include <BLE2902.h>
#endif

// ======================= AJUSTES =======================
const int PIN_REED = 27;                 // sensor da roda (outro fio no GND)
const uint16_t WHEEL_MM_DEFAULT = 2100;  // circunferência padrão em mm (o app pode mudar)

// Bateria: 0 = envia sempre 100% (use se alimentar por power bank USB)
//          1 = lê uma bateria de lítio (3,3 a 4,2 V) por divisor de tensão no PIN_BATTERY.
// Divisor: 100 kΩ do + da bateria ao pino, e 100 kΩ do pino ao GND (divide por 2).
#define USE_BATTERY_ADC 0
const int PIN_BATTERY = 34;

#define BLE_NAME        "BikeSpeed"
#define SERVICE_UUID    "b1cec10c-0001-4e5a-9a3b-5eb1c0de0001"
#define TELEMETRY_UUID  "b1cec10c-0002-4e5a-9a3b-5eb1c0de0001"
#define COMMAND_UUID    "b1cec10c-0003-4e5a-9a3b-5eb1c0de0001"

const uint32_t DEBOUNCE_US   = 15000;    // ignora pulsos com menos de 15 ms
const uint32_t STOP_US       = 3000000;  // sem pulso por 3 s = parado
const uint32_t TELEMETRY_MS  = 250;      // 4 envios por segundo
const float    MOVING_KMH    = 0.5f;     // acima disso conta como em movimento
const float    MAX_VALID_KMH = 90.0f;    // descarta leituras absurdas
// =======================================================

// ---- dados do sensor (mexidos na interrupção) ----
volatile uint32_t lastPulseUs = 0;
volatile uint32_t intervalUs  = 0;       // 0 = ainda sem duas voltas seguidas
volatile uint32_t distMm      = 0;       // distância do percurso em milímetros
volatile uint16_t wheelMm     = WHEEL_MM_DEFAULT;
volatile bool     running     = false;   // percurso em andamento

// ---- estatísticas do percurso ----
float maxKmh  = 0;
float movingS = 0;

// ---- comandos vindos do app (tratados no loop) ----
volatile uint8_t  pendingCmd = 0;
volatile uint32_t pendingArg = 0;

Preferences prefs;
BLECharacteristic* telemetryChar = nullptr;
volatile bool bleConnected = false;

void IRAM_ATTR onPulse() {
  uint32_t now = micros();
  uint32_t dt = now - lastPulseUs;
  if (dt < DEBOUNCE_US) return;
  intervalUs = (dt > STOP_US) ? 0 : dt;  // primeira volta depois de parar não vale como velocidade
  lastPulseUs = now;
  if (running) distMm += wheelMm;
}

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    BLEDevice::startAdvertising();  // volta a ficar visível para reconectar
  }
};

class CommandCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    uint8_t* d = c->getData();
    size_t n = c->getLength();
    if (!d || n < 1) return;
    uint8_t cmd = d[0];
    if (cmd == 0x01 && n >= 5) {
      pendingArg = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
      pendingCmd = 0x01;
    } else if (cmd == 0x02) {
      pendingCmd = 0x02;
    } else if (cmd == 0x03 && n >= 3) {
      pendingArg = (uint32_t)d[1] | ((uint32_t)d[2] << 8);
      pendingCmd = 0x03;
    }
  }
};

void startRide() {
  noInterrupts();
  distMm = 0;
  interrupts();
  maxKmh = 0;
  movingS = 0;
  running = true;
  Serial.println("Percurso iniciado");
}

void endRide() {
  running = false;
  Serial.println("Percurso finalizado");
}

void setWheel(uint16_t mm) {
  if (mm < 1000 || mm > 3000) return;
  wheelMm = mm;
  prefs.putUShort("wheel", mm);
  Serial.printf("Roda: %u mm\n", mm);
}

void handleCommands() {
  uint8_t cmd = pendingCmd;
  if (!cmd) return;
  uint32_t arg = pendingArg;
  pendingCmd = 0;
  if (cmd == 0x01) startRide();
  else if (cmd == 0x02) endRide();
  else if (cmd == 0x03) setWheel((uint16_t)arg);
}

uint8_t readBatteryPercent() {
#if USE_BATTERY_ADC
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BATTERY);
  float mv = (sum / 16.0f) * 2.0f;  // divisor 1:1
  static const float V[] = {3300, 3600, 3700, 3800, 4000, 4200};
  static const float P[] = {0, 15, 35, 55, 85, 100};
  if (mv <= V[0]) return 0;
  if (mv >= V[5]) return 100;
  for (int i = 0; i < 5; i++) {
    if (mv <= V[i + 1]) return (uint8_t)(P[i] + (P[i + 1] - P[i]) * (mv - V[i]) / (V[i + 1] - V[i]));
  }
  return 100;
#else
  return 100;
#endif
}

static void put16(uint8_t* b, int i, uint16_t v) { b[i] = v & 0xFF; b[i + 1] = v >> 8; }
static void put32(uint8_t* b, int i, uint32_t v) {
  b[i] = v & 0xFF; b[i + 1] = (v >> 8) & 0xFF; b[i + 2] = (v >> 16) & 0xFF; b[i + 3] = (v >> 24) & 0xFF;
}

void setupBle() {
  BLEDevice::init(BLE_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEService* service = server->createService(SERVICE_UUID);

  telemetryChar = service->createCharacteristic(TELEMETRY_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
  telemetryChar->addDescriptor(new BLE2902());  // no core 3.x este descritor já é criado sozinho
#endif

  BLECharacteristic* commandChar = service->createCharacteristic(
      COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  commandChar->setCallbacks(new CommandCb());

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Bluetooth ligado, nome: " BLE_NAME);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  prefs.begin("bike", false);
  wheelMm = prefs.getUShort("wheel", WHEEL_MM_DEFAULT);

  pinMode(PIN_REED, INPUT_PULLUP);
#if USE_BATTERY_ADC
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
#endif
  attachInterrupt(digitalPinToInterrupt(PIN_REED), onPulse, FALLING);

  setupBle();
  Serial.printf("Pronto. Roda: %u mm\n", (unsigned)wheelMm);
}

void loop() {
  static uint32_t lastTick = 0;
  static uint32_t lastPrint = 0;
  static uint8_t battery = 100;
  static uint32_t lastBattery = 0;

  handleCommands();

  uint32_t nowMs = millis();
  if (nowMs - lastTick < TELEMETRY_MS) return;
  float dtS = (nowMs - lastTick) / 1000.0f;
  lastTick = nowMs;

  // copia os dados da interrupção de forma segura
  noInterrupts();
  uint32_t iv = intervalUs;
  uint32_t last = lastPulseUs;
  uint32_t dist = distMm;
  uint16_t wheel = wheelMm;
  interrupts();

  // velocidade: circunferência / tempo entre voltas
  float speed = 0;
  uint32_t since = micros() - last;
  if (iv > 0 && since < STOP_US) {
    uint32_t eff = (since > iv) ? since : iv;  // se a roda está desacelerando, a velocidade cai junto
    speed = (wheel / 1000.0f) / (eff / 1000000.0f) * 3.6f;
    if (speed > MAX_VALID_KMH) speed = 0;
  }

  bool isMoving = speed > MOVING_KMH;
  if (running && isMoving) {
    movingS += dtS;
    if (speed > maxKmh) maxKmh = speed;
  }
  float distM = dist / 1000.0f;
  float avg = (movingS > 0) ? (distM / movingS) * 3.6f : 0;

  if (nowMs - lastBattery > 2000 || lastBattery == 0) {
    battery = readBatteryPercent();
    lastBattery = nowMs;
  }

  if (bleConnected && telemetryChar) {
    uint8_t b[12];
    put16(b, 0, (uint16_t)constrain(speed * 10.0f, 0, 65535));
    put16(b, 2, (uint16_t)constrain(avg * 10.0f, 0, 65535));
    put16(b, 4, (uint16_t)constrain(maxKmh * 10.0f, 0, 65535));
    put32(b, 6, (uint32_t)distM);
    b[10] = battery;
    b[11] = (isMoving ? 1 : 0) | (running ? 2 : 0);
    telemetryChar->setValue(b, sizeof(b));
    telemetryChar->notify();
  }

  if (nowMs - lastPrint > 1000) {
    lastPrint = nowMs;
    Serial.printf("vel %.1f km/h | med %.1f | max %.1f | dist %.2f km | bat %u%% | %s | BLE %s\n",
                  speed, avg, maxKmh, distM / 1000.0f, battery,
                  running ? "percurso" : "parado", bleConnected ? "conectado" : "aguardando");
  }
}

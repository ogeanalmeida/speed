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

  Wi-Fi do dia a dia (além do Bluetooth):
    - Informe o Wi-Fi da sua casa em WIFI_SSID / WIFI_PASS abaixo (ou pela página de atualização, veja adiante).
    - Com o celular na mesma rede, abra http://bikespeed.local (ou o IP que aparece no Monitor Serial).
      Abre o PAINEL (status, atualização de firmware, Wi-Fi). O botão "Abrir velocímetro" leva ao app
      (http://bikespeed.local/app), que conecta sozinho pelo Wi-Fi. O Bluetooth continua funcionando
      (use o app pela versão https para isso).
    - Atualizar o firmware a qualquer hora, sem cabo e sem BOOT:
        1) pelo painel: envie o .bin (Sketch > Exportar Binário Compilado) com a senha OTA, ou
        2) pelo Arduino IDE: Ferramentas > Porta > "bikespeed" (rede) e Carregar, com a mesma senha.
      Senha OTA padrão: bike1234 (mude em OTA_PASSWORD abaixo; usuário do painel: admin).

  Atualização por Wi-Fi (OTA):
    - Segure o botão BOOT da placa por 3 segundos com o ESP32 ligado. Ele reinicia em modo atualização
      (o Bluetooth fica desligado nesse modo).
    - Conecte o celular/PC na rede Wi-Fi "BikeSpeed-Update" (senha: bikespeed1) e abra http://192.168.4.1
      (ou, se preencher WIFI_SSID abaixo, http://bikespeed.local na sua rede).
    - Escolha o arquivo .bin (Arduino IDE: Sketch > Exportar Binário Compilado) e envie.
    - Ao terminar, ele reinicia sozinho. Sem enviar nada, volta ao normal em 10 minutos.
    - O primeiro envio deste firmware (que já contém o OTA) precisa ser por USB.

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
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "webapp.h"   // cópia do app para abrir pelo Wi-Fi (gerada por make_webapp.py)
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

// Wi-Fi da sua casa (opcional). Deixe vazio para o ESP32 criar a própria rede no modo atualização.
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* OTA_PASSWORD = "bike1234";   // senha para gravar firmware pelo painel e pelo Arduino IDE
#define FW_VERSION "1.4"
const char* AP_SSID   = "BikeSpeed-Update";
const char* AP_PASS   = "bikespeed1";
const int   PIN_BOOT  = 0;               // botão BOOT da placa
const uint32_t OTA_HOLD_MS    = 3000;    // segurar BOOT por 3 s entra no modo atualização
const uint32_t OTA_TIMEOUT_MS = 600000;  // 10 min sem enviar nada: volta ao normal

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
WebServer web(80);
String wifiSsid, wifiPass;
bool webStarted = false;
bool otaAuthorized = false;
uint8_t telem[12] = {0};   // último pacote de telemetria (usado pelo Bluetooth e pelo Wi-Fi)
bool updateMode = false;
uint32_t updateStartMs = 0;
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

void queueCommand(const uint8_t* d, size_t n) {
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

class CommandCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    queueCommand(c->getData(), c->getLength());
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


// ======================= WI-FI DO DIA A DIA =======================
void loadWifiCreds() {
  wifiSsid = prefs.getString("ssid", WIFI_SSID);
  wifiPass = prefs.getString("pass", WIFI_PASS);
}

void serveAsset(size_t i) {
  if (WEB_ASSETS[i].gz) web.sendHeader("Content-Encoding", "gzip");
  web.sendHeader("Cache-Control", "no-cache");
  web.send_P(200, WEB_ASSETS[i].mime, (const char*)WEB_ASSETS[i].data, WEB_ASSETS[i].len);
}

void registerWebRoutes() {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    web.on(WEB_ASSETS[i].path, HTTP_GET, [i]() { serveAsset(i); });
    if (i == 0) web.on("/", HTTP_GET, [i]() { serveAsset(i); });  // o primeiro arquivo é o index.html
  }
  web.on("/api/t", HTTP_GET, []() {
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("Access-Control-Allow-Origin", "*");
    web.send_P(200, "application/octet-stream", (const char*)telem, sizeof(telem));
  });
  // comandos iguais aos do Bluetooth, em bytes separados por vírgula: /api/cmd?b=1,16,39,0,0
  web.on("/api/cmd", HTTP_POST, []() {
    uint8_t buf[8];
    size_t n = 0;
    String v = web.arg("b");
    int pos = 0;
    while (pos < (int)v.length() && n < sizeof(buf)) {
      int comma = v.indexOf(',', pos);
      if (comma < 0) comma = v.length();
      buf[n++] = (uint8_t)v.substring(pos, comma).toInt();
      pos = comma + 1;
    }
    queueCommand(buf, n);
    web.sendHeader("Access-Control-Allow-Origin", "*");
    web.send(200, "text/plain", "ok");
  });
  web.on("/api/status", HTTP_GET, []() {
    noInterrupts();
    uint32_t dist = distMm;
    uint16_t wheel = wheelMm;
    interrupts();
    char j[360];
    snprintf(j, sizeof(j),
             "{\"versao\":\"%s\",\"build\":\"%s %s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"up\":%lu,"
             "\"heap\":%u,\"ble\":%s,\"wheel\":%u,\"running\":%s,\"dist\":%lu,\"speed\":%.1f}",
             FW_VERSION, __DATE__, __TIME__, WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str(),
             (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), bleConnected ? "true" : "false",
             (unsigned)wheel, running ? "true" : "false", (unsigned long)(dist / 1000),
             (telem[0] | (telem[1] << 8)) / 10.0f);
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", j);
  });

  // gravação de firmware pelo navegador (precisa da senha OTA)
  web.on("/update", HTTP_POST,
    []() {
      if (!otaAuthorized) { web.send(401, "text/plain", "senha OTA errada"); return; }
      bool ok = !Update.hasError();
      web.sendHeader("Connection", "close");
      web.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "falhou ao gravar");
      if (ok) { delay(800); ESP.restart(); }
    },
    []() {
      HTTPUpload& up = web.upload();
      if (up.status == UPLOAD_FILE_START) {
        otaAuthorized = web.authenticate("admin", OTA_PASSWORD);
        if (!otaAuthorized) { Serial.println("OTA web recusado: senha errada"); return; }
        Serial.printf("OTA web: recebendo %s\n", up.filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (!otaAuthorized) {
        return;
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA web ok: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
      }
    });

  web.on("/api/wifi", HTTP_POST, []() {
    if (!web.authenticate("admin", OTA_PASSWORD)) { web.send(401, "text/plain", "senha OTA errada"); return; }
    prefs.putString("ssid", web.arg("ssid"));
    prefs.putString("pass", web.arg("pass"));
    web.send(200, "text/plain", "ok");
    delay(600);
    ESP.restart();
  });
  web.on("/api/reboot", HTTP_POST, []() {
    if (!web.authenticate("admin", OTA_PASSWORD)) { web.send(401, "text/plain", "senha OTA errada"); return; }
    web.send(200, "text/plain", "ok");
    delay(600);
    ESP.restart();
  });
  web.onNotFound([]() { web.send(404, "text/plain", "nao encontrado"); });
}

void startWifiClient() {
  loadWifiCreds();
  if (wifiSsid.length() == 0) {
    Serial.println("Wi-Fi: sem rede configurada (use WIFI_SSID no sketch ou a pagina de atualizacao).");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("bikespeed");
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  registerWebRoutes();
  Serial.printf("Wi-Fi: conectando em \"%s\"...\n", wifiSsid.c_str());
}

// chamada a cada volta do loop: liga o servidor quando o Wi-Fi conecta e atende as requisições
void wifiLoop() {
  if (wifiSsid.length() == 0) return;
  if (!webStarted && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("bikespeed");   // aparece como porta de rede no Arduino IDE (já liga o mDNS)
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();
    MDNS.addService("http", "tcp", 80);
    web.begin();
    webStarted = true;
    Serial.print("Wi-Fi conectado. Abra no celular: http://");
    Serial.print(WiFi.localIP());
    Serial.println(" ou http://bikespeed.local  (painel; /app abre o velocimetro)");
  }
  if (webStarted) {
    web.handleClient();
    ArduinoOTA.handle();
  }
}

// ======================= ATUALIZAÇÃO POR WI-FI =======================
const char UPDATE_PAGE[] PROGMEM =
  "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Bike Speed - Atualizar</title>"
  "<body style='font-family:sans-serif;background:#2f2f2f;color:#fff;padding:24px'>"
  "<h2>Bike Speed - Atualizar firmware</h2>"
  "<form method=POST action=/update enctype=multipart/form-data>"
  "<input type=file name=firmware accept='.bin' required><br><br>"
  "<button style='background:#ff7348;color:#fff3de;border:0;padding:14px 24px;border-radius:12px;font-size:18px'>Enviar</button>"
  "</form><p>Depois de enviar, aguarde: o ESP32 reinicia sozinho.</p>"
  "<hr><h3>Wi-Fi de casa</h3>"
  "<form method=POST action=/wifi>"
  "<input name=ssid placeholder='Nome da rede' required style='width:100%;padding:10px;margin-bottom:8px'><br>"
  "<input name=pass type=password placeholder='Senha' style='width:100%;padding:10px;margin-bottom:8px'><br>"
  "<button style='background:#414141;color:#fff;border:0;padding:12px 20px;border-radius:12px;font-size:16px'>Salvar Wi-Fi</button>"
  "</form>";

void startUpdateMode() {
  updateMode = true;
  updateStartMs = millis();
  WiFi.mode(WIFI_STA);
  bool ok = false;
  loadWifiCreds();
  if (wifiSsid.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(200);
    ok = WiFi.status() == WL_CONNECTED;
  }
  if (ok) {
    Serial.print("Atualizacao: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("Atualizacao: rede ");
    Serial.print(AP_SSID);
    Serial.print(" (senha ");
    Serial.print(AP_PASS);
    Serial.print("), abra http://");
    Serial.println(WiFi.softAPIP());
  }
  MDNS.begin("bikespeed");

  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", UPDATE_PAGE); });
  web.on("/wifi", HTTP_POST, []() {
    prefs.putString("ssid", web.arg("ssid"));
    prefs.putString("pass", web.arg("pass"));
    web.send(200, "text/plain; charset=utf-8", "Wi-Fi salvo. Reiniciando...");
    delay(600);
    ESP.restart();
  });
  web.on("/update", HTTP_POST,
    []() {
      bool fail = Update.hasError();
      web.sendHeader("Connection", "close");
      web.send(200, "text/plain; charset=utf-8", fail ? "FALHOU. Tente de novo." : "OK! Reiniciando...");
      delay(600);
      ESP.restart();
    },
    []() {
      HTTPUpload& up = web.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("Recebendo %s\n", up.filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("Atualizado: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
      }
    });
  web.begin();
}

// segura BOOT por 3 s: marca o modo atualização e reinicia (BOOT segurado durante o reset cairia no bootloader da placa)
void checkUpdateButton() {
  static uint32_t downSince = 0;
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!downSince) downSince = millis();
    if (millis() - downSince > OTA_HOLD_MS) {
      prefs.putBool("ota", true);
      Serial.println("Reiniciando em modo atualizacao...");
      delay(200);
      ESP.restart();
    }
  } else {
    downSince = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  prefs.begin("bike", false);
  wheelMm = prefs.getUShort("wheel", WHEEL_MM_DEFAULT);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (prefs.getBool("ota", false)) {   // pedido de atualização feito antes do reinício
    prefs.putBool("ota", false);
    startUpdateMode();
    return;
  }

  pinMode(PIN_REED, INPUT_PULLUP);
#if USE_BATTERY_ADC
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
#endif
  attachInterrupt(digitalPinToInterrupt(PIN_REED), onPulse, FALLING);

  setupBle();
  startWifiClient();
  Serial.printf("Pronto. Roda: %u mm\n", (unsigned)wheelMm);
}

void loop() {
  if (updateMode) {
    web.handleClient();
    if (millis() - updateStartMs > OTA_TIMEOUT_MS) ESP.restart();
    return;
  }
  checkUpdateButton();
  wifiLoop();

  static uint32_t lastTick = 0;
  static uint32_t lastPrint = 0;
  static uint8_t battery = 100;
  static uint32_t lastBattery = 0;

  handleCommands();

  uint32_t nowMs = millis();
  if (nowMs - lastTick < TELEMETRY_MS) { delay(2); return; }
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

  put16(telem, 0, (uint16_t)constrain(speed * 10.0f, 0, 65535));
  put16(telem, 2, (uint16_t)constrain(avg * 10.0f, 0, 65535));
  put16(telem, 4, (uint16_t)constrain(maxKmh * 10.0f, 0, 65535));
  put32(telem, 6, (uint32_t)distM);
  telem[10] = battery;
  telem[11] = (isMoving ? 1 : 0) | (running ? 2 : 0);
  if (bleConnected && telemetryChar) {
    telemetryChar->setValue(telem, sizeof(telem));
    telemetryChar->notify();
  }

  if (nowMs - lastPrint > 1000) {
    lastPrint = nowMs;
    Serial.printf("vel %.1f km/h | med %.1f | max %.1f | dist %.2f km | bat %u%% | %s | BLE %s | Wi-Fi %s\n",
                  speed, avg, maxKmh, distM / 1000.0f, battery,
                  running ? "percurso" : "parado", bleConnected ? "conectado" : "aguardando",
                  webStarted ? WiFi.localIP().toString().c_str() : "off");
  }
}

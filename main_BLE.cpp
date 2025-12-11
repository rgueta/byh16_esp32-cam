/*
 version con Bluetooth Low Energy (BLE)
 no funciona porque el apagando y encendido
 toma recursos de procesamiento y afecta
 la toma de fotos
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

Preferences preferences;

// Configuración de pines para AI-THINKER ESP32-CAM
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/extra/esp32/fs/sdmmc.h>

using namespace eloq;   // ← SIN ESTO, "camera" NO EXISTE

// Pin del botón físico
#define BUTTON_PIN 12
#define FLASH_PIN        4     // LED Flash/Linterna
#define LED_ROJO 33  // LED rojo en algunas versiones

// Configuración WiFi
const char* ssid = "FamGuEst_2.4";
const char* password = "l0l1t@..:)";
const char* serverHost = "192.168.1.170";
const int serverPort = 5000;
const char* authToken = "Bearer 1234";

// Variables globales
bool LowBrightness = false;
bool buttonPressed = false;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
bool lastButtonState = HIGH;
int photoCounter = 0;
bool sdCardReady = false;
String lastPhotoSDpath = "";

const char* deviceName = "ESP32-CAM-BLE";
String bleAddress = "";

// UUIDs para servicios y características BLE
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_RX "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHARACTERISTIC_UUID_TX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// Variables BLE
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
BLECharacteristic *pRxCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
String bleCommand = "";

void setupWiFi();
bool setupCameraHD();
bool setupSDCard();
bool setupCameraOptimized();
void flushCameraBuffer();
void ledOn(String color);
void ledOff(String color);
void applyLowBrightnessSettings();
String getBLEAddress();
void sendBLEMessage(String message);
void processBluetoothCommand(String command);
void captureProcessAndSend(bool saveToSD, bool sendToServer);
String generarNombreUnico();

// Clase para manejar callbacks del servidor BLE
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("✅ Dispositivo BLE conectado");
      digitalWrite(LED_ROJO, LOW); // LED encendido cuando conectado
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("❌ Dispositivo BLE desconectado");
      digitalWrite(LED_ROJO, HIGH); // LED apagado cuando desconectado
      delay(500); // dar tiempo para que se complete la desconexión
      pServer->startAdvertising(); // volver a anunciar
      Serial.println("📢 BLE anunciando de nuevo...");
    }
};

// Clase para manejar callbacks de característica RX
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();

      if (rxValue.length() > 0) {
        Serial.print("📱 Comando BLE recibido: ");
        Serial.println(rxValue.c_str());

        // Procesar comando
        bleCommand = String(rxValue.c_str());
        bleCommand.trim();
        processBluetoothCommand(bleCommand);
      }
    }
};



void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  Serial.printf("\n=== ESP32-CAM SVGA,10 - CON BLE ===");
  Serial.printf("💾 Memoria inicial: %d bytes\n", ESP.getFreeHeap());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  digitalWrite(FLASH_PIN, LOW); // Asegurar que el flash esté apagado al inicio
  digitalWrite(LED_ROJO, HIGH); // Asegurar que el led rojo esté apagado al inicio

  // Inicializar BLE
  Serial.println("📱 Inicializando BLE...");
  BLEDevice::init(deviceName);
  bleAddress = BLEDevice::getAddress().toString().c_str();
  Serial.printf("📱 Dirección BLE: %s\n", bleAddress.c_str());

  // Crear servidor BLE
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Crear servicio BLE
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Crear característica para TX (envío de datos al cliente)
  pTxCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_TX,
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pTxCharacteristic->addDescriptor(new BLE2902());

  // Crear característica para RX (recepción de comandos desde el cliente)
  pRxCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_RX,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // Iniciar servicio
  pService->start();

  // Configurar parámetros de advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Para evitar problemas de conexión en algunos teléfonos
  pAdvertising->setMinPreferred(0x12);

  // Iniciar advertising
  BLEDevice::startAdvertising();
  Serial.println("📢 BLE anunciando...");
  Serial.println("Conéctate con la app 'BLE Scanner' o similar");

  // WiFi
  setupWiFi();

  // Cámara HD - Calidad reducida para evitar problemas
  Serial.println("📷 Inicializando cámara optimizada...");
  if (!setupCameraOptimized()) {
    Serial.println("❌ Error en cámara!");
    sendBLEMessage("❌ Error en cámara!");
    return;
  }

  // SD Card
  sdCardReady = setupSDCard();

  Serial.println("\n🎯 SISTEMA LISTO");
  Serial.printf("💾 Memoria libre: %d bytes\n", ESP.getFreeHeap());
  sendBLEMessage("🎯 Sistema listo - Envía 'AYUDA' para comandos");

  // Ajustes para reducir brillo central
  if (LowBrightness){
      applyLowBrightnessSettings();
  }

  ledOn("white");
  ledOff("white");
}

String generarNombreUnico() {
    preferences.begin("fotos", false);
    int contador = preferences.getInt("contador", 0);
    contador++;
    preferences.putInt("contador", contador);
    preferences.end();

    return "/fotos/" + String(contador) + ".jpg";
}

void setupWiFi() {
  Serial.printf("📡 Conectando a: %s\n", ssid);
  sendBLEMessage("📡 Conectando WiFi...");

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFi.begin(ssid, password);

  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi conectado!");
      Serial.printf("📶 IP: %s\n", WiFi.localIP().toString().c_str());
      sendBLEMessage("✅ WiFi conectado! IP: " + WiFi.localIP().toString());
      return;
    }
    delay(1000);
    Serial.print(".");
    sendBLEMessage(".");
  }
  Serial.println("\n❌ Error en WiFi");
  sendBLEMessage("❌ Error en WiFi");
}

bool setupCameraOptimized() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 1;

  // Configuración OPTIMIZADA
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA; // 800x600
  config.jpeg_quality = 10; // Calidad media

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Error cámara: 0x%x\n", err);
    return false;
  }

  Serial.printf("✅ Cámara optimizada lista. Memoria: %d bytes\n", ESP.getFreeHeap());
  return true;
}

void applyLowBrightnessSettings() {
    sensor_t *s = esp_camera_sensor_get();

    if (s == NULL) {
        Serial.println("Error: No se pudo obtener sensor");
        return;
    }

    // RESET a valores por defecto primero
    s->set_reg(s, 0xFF, 0x01, 0x01);
    delay(100);

    // Ajustes de cámara para baja luminosidad
    s->set_gain_ctrl(s, 0);      // Control de ganancia OFF
    s->set_exposure_ctrl(s, 1);  // Control de exposición ON
    s->set_aec2(s, 0);           // AEC2 OFF (para exteriores)
    s->set_ae_level(s, -1);      // Nivel exposición: -2 a 2
    s->set_aec_value(s, 800);    // Valor AEC: 300-2000 (menor = más oscuro)
    s->set_agc_gain(s, 0);       // Ganancia AGC: 0-30
    s->set_brightness(s, -1);    // Brillo: -2 a 2
    s->set_contrast(s, 1);       // Contraste: -2 a 2
    s->set_saturation(s, 0);     // Saturación: -2 a 2
    s->set_dcw(s, 1);            // Downsize ENC ON
    s->set_raw_gma(s, 1);        // RAW GMA ON
    s->set_special_effect(s, 0); // Efecto especial: 0=normal

    Serial.println("Configuración de cámara aplicada");
}

bool setupSDCard() {
  if (ESP.getFreeHeap() < 8000) return false;

  if (SD_MMC.begin("/sdcard", true)) {
    if (SD_MMC.cardType() != CARD_NONE) {
      if(!SD_MMC.exists("/fotos")){
        SD_MMC.mkdir("/fotos");
      }
      Serial.println("✅ SD lista");
      sendBLEMessage("✅ SD lista");
      return true;
    }
  }
  return false;
}

bool sendPhotoDirectNoCopy(const uint8_t* imageData, size_t imageSize) {
  Serial.println("\n📡 ENVIANDO FOTO (SIN COPIA)");
  Serial.printf("📏 Tamaño: %d bytes\n", imageSize);
  Serial.printf("💾 Memoria antes: %d bytes\n", ESP.getFreeHeap());

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi no conectado");
    sendBLEMessage("❌ WiFi no conectado");
    return false;
  }

  if (ESP.getFreeHeap() < 12000) {
    Serial.println("⚠️  Memoria baja, pausando...");
    delay(2000);
    Serial.printf("💾 Memoria después de pausa: %d bytes\n", ESP.getFreeHeap());
  }

  WiFiClient client;
  client.setTimeout(30000);

  Serial.println("🔗 Conectando al servidor...");
  sendBLEMessage("🔗 Conectando al servidor...");

  if (!client.connect(serverHost, serverPort)) {
    Serial.println("❌ No se pudo conectar al servidor");
    sendBLEMessage("❌ No se pudo conectar al servidor");
    return false;
  }

  Serial.println("✅ Conectado, preparando envío...");
  sendBLEMessage("✅ Conectado, enviando...");

  String headers = String("POST /upload HTTP/1.1\r\n") +
                  "Host: " + String(serverHost) + ":" + String(serverPort) + "\r\n" +
                  "Content-Type: application/octet-stream\r\n" +
                  "Authorization: " + String(authToken) + "\r\n" +
                  "Content-Length: " + String(imageSize) + "\r\n" +
                  "Connection: close\r\n\r\n";

  if (!client.print(headers)) {
    Serial.println("❌ Error enviando headers");
    client.stop();
    return false;
  }

  size_t sent = 0;
  const size_t CHUNK_SIZE = 512;

  while (sent < imageSize) {
    size_t toSend = (imageSize - sent > CHUNK_SIZE) ? CHUNK_SIZE : (imageSize - sent);
    size_t written = client.write(imageData + sent, toSend);

    if (written == 0) {
      Serial.println("❌ Error enviando chunk");
      Serial.printf("💾 Memoria durante error: %d bytes\n", ESP.getFreeHeap());
      client.stop();
      delay(1000);
      return false;
    }

    sent += written;

    if (sent % 10240 == 0 || sent == imageSize) {
      int progress = (sent * 100) / imageSize;
      Serial.printf("📦 Progreso: %d/%d bytes (%d%%)\n", sent, imageSize, progress);
      sendBLEMessage("📦 Enviando: " + String(progress) + "%");
    }

    delay(1);
  }

  Serial.println("✅ Datos enviados, esperando respuesta...");
  sendBLEMessage("✅ Datos enviados, esperando respuesta...");

  unsigned long startTime = millis();
  while (!client.available() && millis() - startTime < 15000) {
    delay(10);
  }

  bool success = false;
  if (client.available()) {
    String response = client.readStringUntil('\n');
    Serial.printf("📨 Respuesta: %s\n", response.c_str());
    sendBLEMessage("📨 Respuesta: " + response.substring(0, 30));

    if (response.indexOf("200") > 0) {
      success = true;
      Serial.println("🎉 Foto enviada exitosamente!");
      sendBLEMessage("🎉 Foto enviada exitosamente!");
    }
  } else {
    Serial.println("⚠️  Timeout esperando respuesta");
    sendBLEMessage("⚠️  Timeout (datos enviados)");
    if (sent == imageSize) {
      success = true;
      Serial.println("✅ Datos enviados completamente");
    }
  }

  client.stop();
  delay(500);

  return success;
}

bool savePhotoToSD(const uint8_t* imageData, size_t imageSize) {
  if (!sdCardReady) return false;

  String filename = "/fotos/foto_" + String(photoCounter) + "_" + String(millis()) + ".jpg";
  File file = SD_MMC.open(filename, FILE_WRITE);

  if (file) {
    size_t written = file.write(imageData, imageSize);
    file.close();

    if (written == imageSize) {
      lastPhotoSDpath = filename;
      Serial.printf("💾 Guardada en SD: %s (%d bytes)\n", filename.c_str(), imageSize);
      return true;
    }
  }

  return false;
}

void captureProcessAndSend(bool saveToSD, bool sendToServer) {
    photoCounter++;
    Serial.printf("\nTOMANDO FOTO #%d\n", photoCounter);
    Serial.printf("Memoria antes: %d bytes\n", ESP.getFreeHeap());
    sendBLEMessage("📸 Tomando foto #" + String(photoCounter));

    // Limpiar buffer
    flushCameraBuffer();
    delay(100);

    // Disparo de descarte
    Serial.println("📷 Realizando disparo de descarte...");
    if (camera.capture().isOk()) {
        flushCameraBuffer();
    } else {
        Serial.println("⚠️ Fallo el disparo de descarte");
        sendBLEMessage("⚠️ Fallo disparo descarte");
    }
    delay(100);

    // Captura principal
    if (!camera.capture().isOk()) {
        Serial.println("Error capturando foto: " + camera.exception.toString());
        sendBLEMessage("❌ Error capturando foto");
        flushCameraBuffer();
        return;
    }

    // LED indicador
    ledOn("red");
    ledOff("red");

    Serial.printf("Foto capturada: %d bytes\n", camera.frame->len);
    Serial.printf("Memoria tras captura: %d bytes\n", ESP.getFreeHeap());
    sendBLEMessage("✅ Foto: " + String(camera.frame->len/1024) + "KB");

    // Guardar en SD
    if (saveToSD && sdCardReady) {
        char filename[32];
        sprintf(filename, generarNombreUnico().c_str(), photoCounter);

        if (sdmmc.save(camera.frame).to(filename).isOk()) {
            Serial.println("Foto guardada en SD: " + String(filename));
            sendBLEMessage("💾 Guardada: " + String(filename));
        } else {
            Serial.println("Error al guardar en SD");
            sendBLEMessage("❌ Error guardando SD");
        }
    }

    // Enviar al servidor
    if (sendToServer && WiFi.status() == WL_CONNECTED) {
        sendBLEMessage("📡 Enviando al servidor...");
        if (sendPhotoDirectNoCopy(camera.frame->buf, camera.frame->len)) {
            Serial.println("Foto enviada exitosamente!");
        } else {
            Serial.println("Error enviando foto");
            sendBLEMessage("❌ Error enviando foto");
        }
    }

    // Limpiar buffer
    flushCameraBuffer();
    delay(100);

    Serial.printf("Memoria final: %d bytes\n", ESP.getFreeHeap());
    Serial.println("Proceso completado\n");
}

void flushCameraBuffer() {
    if (camera.frame != nullptr) {
        esp_camera_fb_return(camera.frame);
        camera.frame = nullptr;
    }
    Serial.println("Buffer de cámara limpiado");
}

void sendBLEMessage(String message) {
    if (deviceConnected && pTxCharacteristic != NULL) {
        // BLE tiene límite de 20 bytes por paquete, dividimos mensajes largos
        const int CHUNK_SIZE = 20;

        if (message.length() <= CHUNK_SIZE) {
            pTxCharacteristic->setValue(message.c_str());
            pTxCharacteristic->notify();
            delay(10);
        } else {
            // Dividir mensaje largo en chunks
            for (int i = 0; i < message.length(); i += CHUNK_SIZE) {
                String chunk = message.substring(i, i + CHUNK_SIZE);
                pTxCharacteristic->setValue(chunk.c_str());
                pTxCharacteristic->notify();
                delay(10);
            }
        }
    }
}

void processBluetoothCommand(String command) {
  command.toUpperCase();
  command.trim();

  Serial.printf("📱 Comando BLE: %s\n", command.c_str());

  if (!deviceConnected) return;

  if (command == "FOTO") {
    sendBLEMessage("📸 Tomando foto (solo guarda en SD)...");
    captureProcessAndSend(true, false);
  }
  else if (command == "ENVIAR") {
    if (WiFi.status() != WL_CONNECTED) {
      sendBLEMessage("❌ WiFi no conectado");
      return;
    }
    sendBLEMessage("🚀 Tomando y enviando foto ACTUAL...");
    captureProcessAndSend(true, true);
  }
  else if (command == "POST") {
    if (lastPhotoSDpath == "") {
      sendBLEMessage("❌ No hay foto guardada en SD");
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      sendBLEMessage("❌ WiFi no conectado");
      return;
    }
    sendBLEMessage("📡 Enviando última foto de SD...");

    File file = SD_MMC.open(lastPhotoSDpath.c_str(), FILE_READ);
    if (!file) {
      sendBLEMessage("❌ Error abriendo archivo");
      return;
    }

    size_t fileSize = file.size();
    bool success = false;
    WiFiClient client;

    if (client.connect(serverHost, serverPort)) {
      String headers = String("POST /upload HTTP/1.1\r\n") +
                      "Host: " + String(serverHost) + ":" + String(serverPort) + "\r\n" +
                      "Content-Type: application/octet-stream\r\n" +
                      "Authorization: " + String(authToken) + "\r\n" +
                      "Content-Length: " + String(fileSize) + "\r\n" +
                      "Connection: close\r\n\r\n";

      client.print(headers);

      const size_t BUFFER_SIZE = 2048;
      uint8_t buffer[BUFFER_SIZE];
      size_t totalSent = 0;

      while (file.available()) {
        size_t bytesRead = file.read(buffer, BUFFER_SIZE);
        client.write(buffer, bytesRead);
        totalSent += bytesRead;
        delay(2);
      }

      file.close();

      unsigned long start = millis();
      while (!client.available() && millis() - start < 5000) delay(10);

      if (client.available()) {
        String response = client.readStringUntil('\n');
        if (response.indexOf("200") > 0) success = true;
      }

      client.stop();
    } else {
      file.close();
    }

    sendBLEMessage(success ? "✅ Foto de SD enviada" : "❌ Error enviando");
  }
  else if (command == "MEMORIA") {
    String memMsg = "💻 Mem: " + String(ESP.getFreeHeap()) + " bytes\n";
    memMsg += "📡 WiFi: " + String(WiFi.status() == WL_CONNECTED ? "✅" : "❌") + "\n";
    memMsg += "📷 Fotos: " + String(photoCounter) + "\n";
    memMsg += "💾 SD: " + String(sdCardReady ? "✅" : "❌");
    sendBLEMessage(memMsg);
  }
  else if (command == "CALIDAD") {
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
      s->set_quality(s, 22);
      sendBLEMessage("🔧 Calidad reducida a 22");
    }
  }
  else if (command == "OBSCURO"){
      LowBrightness = !LowBrightness;
      Serial.printf("LowBrightness: %s\n", (LowBrightness ? "True" : "False"));
      sendBLEMessage("LowBrightness: " + String(LowBrightness ? "True" : "False"));
  }
  else if (command == "AYUDA"){
      String helpMsg = "BLE: " + bleAddress + "\n";
      helpMsg += "LowBrightness: " + String(LowBrightness ? "True" : "False") + "\n";
      helpMsg += "Comandos: FOTO, ENVIAR, POST\n";
      helpMsg += "MEMORIA, CALIDAD, OBSCURO, AYUDA";
      sendBLEMessage(helpMsg);
  }
  else if (command == "ESTADO" || command == "STATUS") {
      String statusMsg = "=== ESTADO ===\n";
      statusMsg += "BLE: " + String(deviceConnected ? "Conectado" : "Desconectado") + "\n";
      statusMsg += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "No conectado") + "\n";
      statusMsg += "Fotos: " + String(photoCounter) + "\n";
      statusMsg += "Memoria: " + String(ESP.getFreeHeap()) + " bytes";
      sendBLEMessage(statusMsg);
  }
  else {
    sendBLEMessage("❌ Comando no reconocido");
    sendBLEMessage("Usa: FOTO, ENVIAR, POST, MEMORIA, CALIDAD, OBSCURO, AYUDA, ESTADO");
  }
}

void ledOn(String color) {
    if (color == "white"){
        digitalWrite(FLASH_PIN, HIGH);
    }else{
        digitalWrite(LED_ROJO, LOW);
    }
  delay(100);
}

void ledOff(String color) {
    if (color == "white"){
        digitalWrite(FLASH_PIN, LOW);
    }else{
        digitalWrite(LED_ROJO, HIGH);
    }
}

void loop() {
  // Manejar conexiones BLE
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // dar tiempo para que se complete la desconexión
    pServer->startAdvertising();
    Serial.println("📢 BLE anunciando...");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Procesar comandos BLE (ya se procesan en el callback)
  // No necesitamos polling aquí porque BLE usa callbacks

  // Control del botón físico
  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentButtonState == LOW && !buttonPressed) {
      buttonPressed = true;
      Serial.println("🔘 Botón físico presionado");
      if (deviceConnected) {
        sendBLEMessage("🔘 Botón físico presionado");
      }
      captureProcessAndSend(true, true); //guarda en SD y manda al server
    }
    else if (currentButtonState == HIGH && buttonPressed) {
      buttonPressed = false;
    }
  }

  lastButtonState = currentButtonState;

  delay(100);
}

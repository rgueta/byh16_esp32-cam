#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "BluetoothSerial.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include <WiFi.h>

// Configuración de pines para AI-THINKER ESP32-CAM
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#include <eloquent_esp32cam.h>
#include <eloquent_esp32cam/extra/esp32/fs/sdmmc.h>

// ESTO ES LO QUE TE FALTABA:
using namespace eloq;   // ← SIN ESTO, "camera" NO EXISTE

// Pin del botón físico
#define BUTTON_PIN 12
#define FLASH_PIN        4     // LED Flash/Linterna

// Configuración WiFi
const char* ssid = "FamGuEst_2.4";
const char* password = "l0l1t@..:)";
const char* serverHost = "192.168.1.170";
const int serverPort = 5000;
const char* authToken = "Bearer 1234";

// Objeto Bluetooth
BluetoothSerial SerialBT;

// Variables globales
bool LowBrightness = false;
bool buttonPressed = false;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;
bool lastButtonState = HIGH;
int photoCounter = 0;
bool bluetoothEnabled = false;
bool sdCardReady = false;
String lastPhotoSDpath = "";

const char* deviceName = "BYH16PIC1";
String macAddress = "";
String btMAC = "";

void setupWiFi();
bool setupCameraHD();
bool setupSDCard();
bool setupCameraOptimized();
void flushCameraBuffer();
void flashOn();
void flashOff();
void applyLowBrightnessSettings();
String getBluetoothMAC();


// NO usamos buffer temporal - demasiada memoria
// En su lugar, enviamos directamente desde el frame buffer

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  Serial.printf("\n=== ESP32-CAM SVGA,10 - SIN BUFFER TEMPORAL ===");

  Serial.printf("💾 Memoria inicial: %d bytes\n", ESP.getFreeHeap());

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW); // Asegurar que el flash esté apagado al inicio

  // Bluetooth - Solo si hay suficiente memoria
  if (bluetoothEnabled) {
    if (!SerialBT.begin(deviceName)) {
      Serial.println("❌ Error en Bluetooth");
      bluetoothEnabled = false;
    } else {
      btMAC = getBluetoothMAC();
      Serial.printf("✅ Bluetooth: %s, %s\n ", deviceName, btMAC.c_str());
    }
  }

  // WiFi
  setupWiFi();

  // Cámara HD - Calidad reducida para evitar problemas
  Serial.println("📷 Inicializando cámara optimizada...");
  if (!setupCameraOptimized()) {
    Serial.println("❌ Error en cámara!");
    return;
  }

  // SD Card
  sdCardReady = setupSDCard();

  Serial.println("\n🎯 SISTEMA LISTO");
  Serial.printf("💾 Memoria libre: %d bytes\n", ESP.getFreeHeap());
  Serial.println("📱 ENVIAR: Toma y envía foto ACTUAL");
  Serial.println("📱 POST: Envía última foto de SD");
  Serial.printf("LowBrightness: %s\n", (LowBrightness ? "True" : "False"));


  // Ajustes para reducir brillo central
  if (LowBrightness){
      applyLowBrightnessSettings();
  }

  flashOn();
  flashOff();
}

void setupWiFi() {
  Serial.printf("📡 Conectando a: %s\n", ssid);

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFi.begin(ssid, password);

  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi conectado!");
      Serial.printf("📶 IP: %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\n❌ Error en WiFi");
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

  // Configuración OPTIMIZADA - Menos calidad para fotos más pequeñas
  config.pixel_format = PIXFORMAT_JPEG;
  // config.frame_size = FRAMESIZE_XGA; // 1024x768
  // config.jpeg_quality = 24; // Calidad alta

  config.frame_size = FRAMESIZE_SVGA; // 800x600
  config.jpeg_quality = 20; // Calidad media (menos que antes)

  // config.frame_size = FRAMESIZE_VGA; // 640x480
  // config.jpeg_quality = 10; // 10-63, lower means higher quality

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

    // 1. Deshabilitar ajustes automáticos problemáticos
    s->set_gain_ctrl(s, 0);      // Control de ganancia OFF
    s->set_exposure_ctrl(s, 1);  // Control de exposición ON
    s->set_aec2(s, 0);           // AEC2 OFF (para exteriores)

    // 2. Ajustar exposición manualmente
    s->set_ae_level(s, -1);      // Nivel exposición: -2 a 2
    s->set_aec_value(s, 800);    // Valor AEC: 300-2000 (menor = más oscuro)

    // 3. Ajustar ganancia
    s->set_agc_gain(s, 0);       // Ganancia AGC: 0-30

    // 4. Ajustes básicos de imagen
    s->set_brightness(s, -1);    // Brillo: -2 a 2
    s->set_contrast(s, 1);       // Contraste: -2 a 2
    s->set_saturation(s, 0);     // Saturación: -2 a 2

    // 5. Configuraciones adicionales
    s->set_dcw(s, 1);            // Downsize ENC ON
    s->set_raw_gma(s, 1);        // RAW GMA ON

    // 6. Específico para OV2640 (modelo común)
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
    return false;
  }

  // Si la memoria está muy baja, hacer una pausa
  if (ESP.getFreeHeap() < 12000) {
    Serial.println("⚠️  Memoria baja, pausando...");
    delay(2000);
    Serial.printf("💾 Memoria después de pausa: %d bytes\n", ESP.getFreeHeap());
  }

  WiFiClient client;
  client.setTimeout(30000); // Timeout largo

  Serial.println("🔗 Conectando al servidor...");

  if (!client.connect(serverHost, serverPort)) {
    Serial.println("❌ No se pudo conectar al servidor");
    return false;
  }

  Serial.println("✅ Conectado, preparando envío...");

  // Construir request HTTP
  String headers = String("POST /upload HTTP/1.1\r\n") +
                  "Host: " + String(serverHost) + ":" + String(serverPort) + "\r\n" +
                  "Content-Type: application/octet-stream\r\n" +
                  "Authorization: " + String(authToken) + "\r\n" +
                  "Content-Length: " + String(imageSize) + "\r\n" +
                  "Connection: close\r\n\r\n";

  // Enviar headers
  if (!client.print(headers)) {
    Serial.println("❌ Error enviando headers");
    client.stop();
    return false;
  }

  // Enviar datos DIRECTAMENTE desde imageData - SIN COPIA
  size_t sent = 0;
  const size_t CHUNK_SIZE = 512; // Chunks más pequeños para memoria baja
   // const size_t CHUNK_SIZE = 1024; // Chunks medio para prueba de memoria
  //const size_t CHUNK_SIZE = 2048; // Chunks MAxima para prueba, evitar fallas de envio


  while (sent < imageSize) {
    size_t toSend = (imageSize - sent > CHUNK_SIZE) ? CHUNK_SIZE : (imageSize - sent);

    // Enviar chunk directamente desde imageData
    size_t written = client.write(imageData + sent, toSend);

    if (written == 0) {
      Serial.println("❌ Error enviando chunk - puede ser memoria baja");
      Serial.printf("💾 Memoria durante error: %d bytes\n", ESP.getFreeHeap());
      client.stop();

      // Intentar liberar memoria
      delay(1000);
      return false;
    }

    sent += written;

    // Mostrar progreso cada 10KB
    if (sent % 10240 == 0 || sent == imageSize) {
      Serial.printf("📦 Progreso: %d/%d bytes (%.1f%%)\n",
                   sent, imageSize, (sent * 100.0) / imageSize);
      Serial.printf("💾 Memoria durante envío: %d bytes\n", ESP.getFreeHeap());
    }

    // Pequeña pausa para evitar saturación y dar tiempo al sistema
    delay(1);
  }

  Serial.println("✅ Datos enviados, esperando respuesta...");

  // Esperar respuesta con timeout
  unsigned long startTime = millis();
  while (!client.available() && millis() - startTime < 15000) {
    delay(10);
  }

  bool success = false;
  if (client.available()) {
    String response = client.readStringUntil('\n');
    Serial.printf("📨 Respuesta: %s\n", response.c_str());

    if (response.indexOf("200") > 0) {
      success = true;
      Serial.println("🎉 Foto enviada exitosamente!");
    }
  } else {
    Serial.println("⚠️  Timeout esperando respuesta (pero datos enviados)");
    // Considerar éxito si los datos se enviaron completamente
    if (sent == imageSize) {
      success = true;
      Serial.println("✅ Datos enviados completamente, asumiendo éxito");
    }
  }

  client.stop();

  // Pausa para liberar memoria después del envío
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

    // LIMPIAR BUFFER ANTES DE CAPTURAR (crucial!)
    flushCameraBuffer();
    delay(100);  // Estabilización

    // =======================================================
    // ⭐️ PASO NUEVO: DISPARO DE DESCARTE (DUMMY SHOT / HARDWARE FLUSH)
    // Se fuerza al hardware a tomar una foto y llenar el buffer con datos actuales.
    // Esto asegura que el siguiente 'capture()' tome el frame más reciente.
    // =======================================================
    Serial.println("📷 Realizando disparo de descarte...");
    if (camera.capture().isOk()) {
        // Liberar el frame de descarte inmediatamente
        flushCameraBuffer();
    } else {
        Serial.println("⚠️ Fallo el disparo de descarte. Intentando continuar.");
    }
    delay(100); // Pequeña pausa para estabilización



    // CAPTURA USANDO ELOQUENT (NO uses esp_camera_fb_get() directamente)
    if (!camera.capture().isOk()) {
        Serial.println("Error capturando foto: " + camera.exception.toString());
        flushCameraBuffer();  // Limpia aunque haya fallado
        return;
    }

    // Ahora el frame está en camera.frame (seguro y limpio)
    Serial.printf("Foto capturada: %d bytes\n", camera.frame->len);
    Serial.printf("Memoria tras captura: %d bytes\n", ESP.getFreeHeap());

    // GUARDAR EN SD (si se solicita)
    if (saveToSD) {
        char filename[32];
        sprintf(filename, "/fotos/IMG_%04d.jpg", photoCounter);

        if (sdmmc.save(camera.frame).to(filename).isOk()) {
            Serial.println("Foto guardada en SD: " + String(filename));
        } else {
            Serial.println("Error al guardar en SD");
        }
    }

    // ENVIAR AL SERVIDOR (directo desde buffer, sin copias)
    if (sendToServer && WiFi.status() == WL_CONNECTED) {
        Serial.println("Enviando foto al servidor...");

        if (sendPhotoDirectNoCopy(camera.frame->buf, camera.frame->len)) {
            Serial.println("Foto enviada exitosamente!");
            if (bluetoothEnabled && SerialBT.hasClient()) {
                SerialBT.println("Foto enviada al servidor!");
            }
        } else {
            Serial.println("Error enviando foto");
            if (bluetoothEnabled && SerialBT.hasClient()) {
                SerialBT.println("Error enviando foto");
            }
        }
    }

    // LIMPIAR BUFFER AL FINAL (¡OBLIGATORIO!)
    flushCameraBuffer();
    delay(100);

    Serial.printf("Memoria final: %d bytes\n", ESP.getFreeHeap());
    Serial.println("Proceso completado\n");
}

void flushCameraBuffer() {
    // Método compatible con ambas librerías
    if (camera.frame != nullptr) {
        esp_camera_fb_return(camera.frame);
        camera.frame = nullptr;
    }

    // Si usas EloquentEsp32cam, también puedes hacer:
    // camera.frame->clear();

    Serial.println("Buffer de cámara limpiado");
}

void processBluetoothCommand(String command) {
  command.toUpperCase();
  command.trim();

  Serial.printf("📱 Comando: %s\n", command.c_str());

  if (!bluetoothEnabled || !SerialBT.hasClient()) return;

  if (command == "FOTO") {
    SerialBT.println("📸 Tomando foto (solo guarda en SD)...");
    captureProcessAndSend(true, false);
  }
  else if (command == "ENVIAR") {

  }
  else if (command == "ENVIAR") {
    if (WiFi.status() != WL_CONNECTED) {
      SerialBT.println("❌ WiFi no conectado");
      return;
    }
    SerialBT.println("🚀 Tomando y enviando foto ACTUAL...");
    captureProcessAndSend(true, true);
  }
  else if (command == "POST") {
    if (lastPhotoSDpath == "") {
      SerialBT.println("❌ No hay foto guardada en SD");
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      SerialBT.println("❌ WiFi no conectado");
      return;
    }
    SerialBT.println("📡 Enviando última foto de SD...");

    File file = SD_MMC.open(lastPhotoSDpath.c_str(), FILE_READ);
    if (!file) {
      SerialBT.println("❌ Error abriendo archivo");
      return;
    }

    size_t fileSize = file.size();

    // Leer en chunks para no usar mucha memoria
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

      // Esperar respuesta breve
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

    SerialBT.println(success ? "✅ Foto de SD enviada" : "❌ Error enviando");
  }
  else if (command == "MEMORIA") {
    SerialBT.printf("💻 Memoria: %d bytes\n", ESP.getFreeHeap());
    SerialBT.printf("📡 WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "✅" : "❌");
    SerialBT.printf("📷 Fotos: %d\n", photoCounter);
    SerialBT.printf("💾 SD: %s\n", sdCardReady ? "✅" : "❌");
  }
  else if (command == "CALIDAD") {
    // Cambiar a calidad más baja si hay problemas
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
      s->set_quality(s, 22); // Calidad más baja
      SerialBT.println("🔧 Calidad reducida a 22 para fotos más pequeñas");
    }
  }
  else if (command == "OBSCURO"){
      LowBrightness = !LowBrightness;
      Serial.printf("LowBrightness: %s\n", (LowBrightness ? "True" : "False"));
      SerialBT.printf("LowBrightness: %s\n", (LowBrightness ? "True" : "False"));
  }

  else if (command == "AYUDA"){
      SerialBT.printf("BT MAC: %s\n", btMAC.c_str());
      SerialBT.printf("LowBrightness: %s\n", (LowBrightness ? "True" : "False"));
  }
  else {
    SerialBT.println("❌ Comando no reconocido");
    SerialBT.println("Comandos: FOTO, ENVIAR, POST, MEMORIA, CALIDAD, OBSCURO");
  }
}

// Función para obtener MAC Bluetooth
String getBluetoothMAC() {
  const uint8_t* point = esp_bt_dev_get_address();

  if (point == NULL) {
    return "00:00:00:00:00:00";
  }

  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          point[0], point[1], point[2],
          point[3], point[4], point[5]);

  return String(macStr);
}


void flashOn() {
  digitalWrite(FLASH_PIN, HIGH);
  delay(100);
}

void flashOff() {
  digitalWrite(FLASH_PIN, LOW);
}

void loop() {
  if (bluetoothEnabled && SerialBT.hasClient() && SerialBT.available()) {
    String command = SerialBT.readString();
    command.trim();
    processBluetoothCommand(command);
  }

  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentButtonState == LOW && !buttonPressed) {
      buttonPressed = true;
      SerialBT.println("🔘 Botón físico presionado");
      Serial.println("🔘 Botón físico presionado");
      captureProcessAndSend(true, false); //guarda en SD y manda al server
    }
    else if (currentButtonState == HIGH && buttonPressed) {
      buttonPressed = false;
    }
  }

  lastButtonState = currentButtonState;

  delay(100);
}

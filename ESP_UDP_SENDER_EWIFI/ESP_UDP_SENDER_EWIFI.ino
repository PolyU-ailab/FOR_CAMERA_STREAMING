#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_camera.h"
#include "esp_wifi.h"

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include "esp_eap_client.h"
#else
  #include "esp_wpa2.h"
#endif

// =====================
// Enterprise WiFi Config
// =====================
const char* WIFI_SSID = "STEM";

// Replace these with your enterprise WiFi login details
const char* EAP_IDENTITY = "gststem01";
const char* EAP_USERNAME = "gststem01";
const char* EAP_PASSWORD = "A1b2c3d4";

// =====================
// Receiver Config
// =====================
const char* SERVER_IP = "95.40.85.248";

// For cam1 use 5005
// For cam2 use 5006
// For cam3 use 5007
const uint16_t SERVER_PORT = 5010;

// =====================
// UDP Config
// =====================
WiFiUDP udp;

// Keep UDP payload below MTU.
// 1200 is usually safe over WiFi.
static const uint16_t UDP_CHUNK_SIZE = 1200;

// Packet header:
// magic      4 bytes
// frame_id   4 bytes
// total_len  4 bytes
// offset     4 bytes
// chunk_len  2 bytes
//
// Total header size = 18 bytes
struct __attribute__((packed)) UdpJpegHeader {
  uint32_t magic;
  uint32_t frame_id;
  uint32_t total_len;
  uint32_t offset;
  uint16_t chunk_len;
};

static const uint32_t MAGIC = 0x31474D4A; // "JMG1"
uint32_t frameCounter = 0;

// =====================
// Camera Pin Config
// =====================
// IMPORTANT:
// These pins must match your ESP32-S3 camera module.
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

void connectEnterpriseWiFi() {
  WiFi.disconnect(true);
  delay(1000);

  WiFi.mode(WIFI_STA);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino ESP32 core 3.x / ESP-IDF 5.x
  esp_eap_client_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_eap_client_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_eap_client_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));

  // CA certificate not used because CA Certification = Do not validate
  esp_wifi_sta_enterprise_enable();

#else
  // Arduino ESP32 core 2.x
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)EAP_IDENTITY, strlen(EAP_IDENTITY));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)EAP_USERNAME, strlen(EAP_USERNAME));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)EAP_PASSWORD, strlen(EAP_PASSWORD));

  // CA certificate not used because CA Certification = Do not validate
  esp_wifi_sta_wpa2_ent_enable();
#endif

  WiFi.begin(WIFI_SSID);

  Serial.print("Connecting to enterprise WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

void initCamera() {
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

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Safer settings to avoid camera capture failed
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 18;

  // Safer buffer settings
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Camera initialized");

  // Give camera sensor time to stabilize
  delay(1000);
}

void sendJpegUdp(const uint8_t* jpg, uint32_t len) {
  uint32_t frameId = frameCounter++;

  for (uint32_t offset = 0; offset < len; offset += UDP_CHUNK_SIZE) {
    uint16_t chunkLen = UDP_CHUNK_SIZE;

    if (offset + chunkLen > len) {
      chunkLen = len - offset;
    }

    UdpJpegHeader hdr;
    hdr.magic = MAGIC;
    hdr.frame_id = frameId;
    hdr.total_len = len;
    hdr.offset = offset;
    hdr.chunk_len = chunkLen;

    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.write((uint8_t*)&hdr, sizeof(hdr));
    udp.write(jpg + offset, chunkLen);
    udp.endPacket();

    // Small delay helps reduce UDP packet loss on some WiFi networks.
    delayMicroseconds(300);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectEnterpriseWiFi();

  initCamera();

  udp.begin(0);

  Serial.printf("Sending camera stream to %s:%u\n", SERVER_IP, SERVER_PORT);
}

void loop() {
  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed");
    delay(100);
    return;
  }

  if (fb->format == PIXFORMAT_JPEG) {
    sendJpegUdp(fb->buf, fb->len);
    Serial.printf("Sent frame %lu, size: %u bytes\n",
                  (unsigned long)frameCounter,
                  (unsigned int)fb->len);
  } else {
    Serial.println("Frame is not JPEG");
  }

  esp_camera_fb_return(fb);

  // Frame rate control.
  // Increase this if packets drop.
  delay(50);
}

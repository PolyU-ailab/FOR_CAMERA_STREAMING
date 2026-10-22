#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>

#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_arduino_version.h"

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include "esp_eap_client.h"
#else
  #include "esp_wpa2.h"
#endif

// ======================================================
// WPA2-ENTERPRISE WIFI SETTINGS
// ======================================================

const char* WIFI_SSID = "PolyUWLAN";

// Replace these placeholders with your new credentials.
const char* EAP_IDENTITY = "cw3chan";
const char* EAP_USERNAME = "cw3chan";
const char* EAP_PASSWORD = "aA55933936";

// Restart the ESP32 if Wi-Fi cannot connect after this
// number of attempts.
static const uint16_t WIFI_MAX_ATTEMPTS = 60;

// ======================================================
// MQTT SETTINGS
// ======================================================

const char* MQTT_SERVER = "43.198.136.97";
const uint16_t MQTT_PORT = 1883;

// Commands are received from this topic.
const char* MQTT_TOPIC = "POLYU/INTERFERENCE/set_1/cam";

// This ID must be unique for every device connected to
// the MQTT broker.
const char* MQTT_CLIENT_ID = "POLYU/INTERFERENCE/set_1/cam";

// Leave empty when the MQTT broker allows anonymous access.
const char* MQTT_USERNAME = "";
const char* MQTT_PASSWORD = "";

// Wait this long between MQTT reconnection attempts.
static const uint32_t MQTT_RETRY_INTERVAL_MS = 5000;

// ======================================================
// UDP CAMERA RECEIVER SETTINGS
// ======================================================

const char* SERVER_IP = "43.199.61.48";
const uint16_t SERVER_PORT = 5053;

// Keep each UDP data chunk below the typical network MTU.
static const uint16_t UDP_CHUNK_SIZE = 1200;

// Delay between UDP packets to reduce packet loss.
static const uint16_t UDP_PACKET_DELAY_US = 300;

// ======================================================
// FRAME INTERVAL SETTINGS
// ======================================================
//
// FRAME_INTERVAL_MS is controlled through MQTT.
//
// Examples:
//
// FRAMERATE|200  = approximately 5 FPS
// FRAMERATE|100  = approximately 10 FPS
// FRAMERATE|50   = approximately 20 FPS
//
// This is the desired minimum interval between frames.
// Actual FPS may be lower because of camera capture,
// JPEG compression, UDP transfer, and Wi-Fi performance.
//

uint32_t FRAME_INTERVAL_MS = 200;

// Accepted FRAME_INTERVAL_MS range.
static const uint32_t MIN_FRAME_INTERVAL_MS = 1;
static const uint32_t MAX_FRAME_INTERVAL_MS = 60000;

// ======================================================
// UDP JPEG PACKET HEADER
// ======================================================
//
// Packet header:
// magic      4 bytes
// frame_id   4 bytes
// total_len  4 bytes
// offset     4 bytes
// chunk_len  2 bytes
//
// Total header size = 18 bytes
//

struct __attribute__((packed)) UdpJpegHeader
{
    uint32_t magic;
    uint32_t frame_id;
    uint32_t total_len;
    uint32_t offset;
    uint16_t chunk_len;
};

static const uint32_t MAGIC = 0x31474D4A; // "JMG1"

// ======================================================
// ESP32-S3 CAMERA PIN CONFIGURATION
// ======================================================
//
// IMPORTANT:
// These pins must match your ESP32-S3 camera module.
//

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

// ======================================================
// NETWORK OBJECTS
// ======================================================

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
WiFiUDP udp;

// ======================================================
// GLOBAL STATE
// ======================================================

// Streaming is disabled by default.
bool cameraStreaming = false;

// Incremented for every transmitted JPEG frame.
uint32_t frameCounter = 0;

// Frame-rate control.
uint32_t lastFrameTime = 0;

// MQTT reconnection timing.
uint32_t lastMqttReconnectAttempt = 0;

// ======================================================
// PROCESS FRAMERATE COMMAND
// ======================================================

void processFrameRateCommand(const String& message)
{
    // Expected format:
    //
    // FRAMERATE|200

    static const String prefix = "FRAMERATE|";

    String valueText = message.substring(prefix.length());
    valueText.trim();

    if (valueText.length() == 0)
    {
        Serial.println("--------------------------------");
        Serial.println("INVALID FRAME INTERVAL");
        Serial.println("No number was provided.");
        Serial.println("Example: FRAMERATE|200");
        Serial.println("--------------------------------");
        return;
    }

    // strtol allows invalid values such as "abc" and "20abc"
    // to be rejected instead of silently converting them.
    char* endPointer = nullptr;

    long decodedValue = strtol(
        valueText.c_str(),
        &endPointer,
        10
    );

    const bool isValidInteger =
        endPointer != valueText.c_str() &&
        *endPointer == '\0';

    const bool isWithinRange =
        decodedValue >= static_cast<long>(MIN_FRAME_INTERVAL_MS) &&
        decodedValue <= static_cast<long>(MAX_FRAME_INTERVAL_MS);

    if (!isValidInteger)
    {
        Serial.println("--------------------------------");
        Serial.println("INVALID FRAME INTERVAL");

        Serial.print("Received value: ");
        Serial.println(valueText);

        Serial.println("The value must be a whole integer.");
        Serial.println("Example: FRAMERATE|200");
        Serial.println("--------------------------------");
        return;
    }

    if (!isWithinRange)
    {
        Serial.println("--------------------------------");
        Serial.println("FRAME INTERVAL OUT OF RANGE");

        Serial.print("Received value: ");
        Serial.println(decodedValue);

        Serial.print("Minimum interval: ");
        Serial.print(MIN_FRAME_INTERVAL_MS);
        Serial.println(" ms");

        Serial.print("Maximum interval: ");
        Serial.print(MAX_FRAME_INTERVAL_MS);
        Serial.println(" ms");

        Serial.println("--------------------------------");
        return;
    }

    FRAME_INTERVAL_MS = static_cast<uint32_t>(decodedValue);

    // Reset frame timing so the new interval begins immediately.
    lastFrameTime = millis();

    Serial.println("--------------------------------");
    Serial.println("FRAME INTERVAL UPDATED");

    Serial.print("New frame interval: ");
    Serial.print(FRAME_INTERVAL_MS);
    Serial.println(" ms");

    Serial.print("Approximate maximum FPS: ");
    Serial.println(
        1000.0f / static_cast<float>(FRAME_INTERVAL_MS),
        2
    );

    Serial.println("--------------------------------");
}

// ======================================================
// MQTT CALLBACK
// ======================================================
//
// Supported MQTT payloads:
//
// STREAM|1
// STREAM|0
// FRAMERATE|x
//
// Examples:
//
// FRAMERATE|200
// FRAMERATE|100
// FRAMERATE|50
//

void mqttCallback(
    char* topic,
    byte* payload,
    unsigned int length
)
{
    Serial.println();
    Serial.println("================================");
    Serial.println("MQTT MESSAGE RECEIVED");
    Serial.println("================================");

    Serial.print("Topic: ");
    Serial.println(topic);

    String message;
    message.reserve(length);

    for (unsigned int i = 0; i < length; i++)
    {
        message += static_cast<char>(payload[i]);
    }

    // Remove accidental spaces, tabs, and line endings
    // from the beginning and end of the message.
    message.trim();

    Serial.print("Payload: ");
    Serial.println(message);

    Serial.print("Payload length: ");
    Serial.println(length);

    // --------------------------------------------------
    // Start streaming
    // --------------------------------------------------

    if (message == "STREAM|1")
    {
        cameraStreaming = true;

        // Setting this to zero causes the next frame to be
        // captured immediately.
        lastFrameTime = 0;

        Serial.println("--------------------------------");
        Serial.println("CAMERA STREAM STARTED");

        Serial.print("Frame interval: ");
        Serial.print(FRAME_INTERVAL_MS);
        Serial.println(" ms");

        Serial.print("Approximate maximum FPS: ");
        Serial.println(
            1000.0f / static_cast<float>(FRAME_INTERVAL_MS),
            2
        );

        Serial.println("--------------------------------");
    }

    // --------------------------------------------------
    // Stop streaming
    // --------------------------------------------------

    else if (message == "STREAM|0")
    {
        cameraStreaming = false;

        Serial.println("--------------------------------");
        Serial.println("CAMERA STREAM STOPPED");
        Serial.println("--------------------------------");
    }

    // --------------------------------------------------
    // Change frame interval
    // --------------------------------------------------

    else if (message.startsWith("FRAMERATE|"))
    {
        processFrameRateCommand(message);
    }

    // --------------------------------------------------
    // Unknown command
    // --------------------------------------------------

    else
    {
        Serial.println("--------------------------------");
        Serial.println("UNKNOWN MQTT COMMAND");
        Serial.println();
        Serial.println("Valid commands:");
        Serial.println("STREAM|1");
        Serial.println("STREAM|0");
        Serial.println("FRAMERATE|x");
        Serial.println();
        Serial.println("Examples:");
        Serial.println("FRAMERATE|200");
        Serial.println("FRAMERATE|100");
        Serial.println("FRAMERATE|50");
        Serial.println("--------------------------------");
    }

    Serial.println("================================");
}

// ======================================================
// CONFIGURE WPA2-ENTERPRISE AUTHENTICATION
// ======================================================

void configureEnterpriseAuthentication()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3

    // Arduino-ESP32 Core 3.x / ESP-IDF 5.x

    esp_eap_client_set_identity(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_IDENTITY)
        ),
        strlen(EAP_IDENTITY)
    );

    esp_eap_client_set_username(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_USERNAME)
        ),
        strlen(EAP_USERNAME)
    );

    esp_eap_client_set_password(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_PASSWORD)
        ),
        strlen(EAP_PASSWORD)
    );

    // No CA certificate is configured.
    esp_wifi_sta_enterprise_enable();

#else

    // Arduino-ESP32 Core 2.x

    esp_wifi_sta_wpa2_ent_set_identity(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_IDENTITY)
        ),
        strlen(EAP_IDENTITY)
    );

    esp_wifi_sta_wpa2_ent_set_username(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_USERNAME)
        ),
        strlen(EAP_USERNAME)
    );

    esp_wifi_sta_wpa2_ent_set_password(
        reinterpret_cast<uint8_t*>(
            const_cast<char*>(EAP_PASSWORD)
        ),
        strlen(EAP_PASSWORD)
    );

    // No CA certificate is configured.
    esp_wifi_sta_wpa2_ent_enable();

#endif
}

// ======================================================
// CONNECT TO WPA2-ENTERPRISE WIFI
// ======================================================

void connectEnterpriseWiFi()
{
    Serial.println();
    Serial.println("--------------------------------");
    Serial.println("Connecting to WPA2 Enterprise");
    Serial.println("--------------------------------");

    // Stop camera streaming during Wi-Fi reconnection.
    cameraStreaming = false;

    WiFi.disconnect(true);
    delay(1000);

    WiFi.mode(WIFI_STA);

    // Disable Wi-Fi power saving to reduce UDP latency
    // and packet loss during streaming.
    WiFi.setSleep(false);

    configureEnterpriseAuthentication();

    WiFi.begin(WIFI_SSID);

    Serial.print("Connecting");

    uint16_t attempt = 0;

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");

        attempt++;

        if (attempt >= WIFI_MAX_ATTEMPTS)
        {
            Serial.println();
            Serial.println("Wi-Fi connection timed out.");
            Serial.println("Restarting ESP32...");

            delay(2000);
            ESP.restart();
        }
    }

    Serial.println();
    Serial.println("Wi-Fi connected successfully!");

    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    Serial.println("--------------------------------");
}

// ======================================================
// CONNECT TO MQTT SERVER
// ======================================================

bool connectMQTT()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return false;
    }

    if (mqttClient.connected())
    {
        return true;
    }

    Serial.println();
    Serial.print("Connecting to MQTT broker: ");
    Serial.print(MQTT_SERVER);
    Serial.print(":");
    Serial.println(MQTT_PORT);

    bool connected = false;

    if (strlen(MQTT_USERNAME) > 0)
    {
        connected = mqttClient.connect(
            MQTT_CLIENT_ID,
            MQTT_USERNAME,
            MQTT_PASSWORD
        );
    }
    else
    {
        connected = mqttClient.connect(
            MQTT_CLIENT_ID
        );
    }

    if (!connected)
    {
        Serial.print("MQTT connection failed. State = ");
        Serial.println(mqttClient.state());

        return false;
    }

    Serial.println("MQTT connected successfully!");

    if (!mqttClient.subscribe(MQTT_TOPIC))
    {
        Serial.println("MQTT topic subscription failed.");

        mqttClient.disconnect();
        return false;
    }

    Serial.print("Subscribed to MQTT topic: ");
    Serial.println(MQTT_TOPIC);

    Serial.println("Waiting for camera commands...");

    return true;
}

// ======================================================
// INITIALIZE CAMERA
// ======================================================

void initCamera()
{
    camera_config_t config = {};

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

    config.xclk_freq_hz = 40000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Default settings when PSRAM is available.
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 18;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    // Use lower settings if PSRAM is unavailable.
    if (!psramFound())
    {
        Serial.println("WARNING: PSRAM was not detected.");
        Serial.println("Using VGA resolution and internal DRAM.");

        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 20;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }
    else
    {
        Serial.println("PSRAM detected.");
    }

    esp_err_t error = esp_camera_init(&config);

    if (error != ESP_OK)
    {
        Serial.printf(
            "Camera initialization failed. Error: 0x%x\n",
            error
        );

        Serial.println("ESP32 stopped because the camera is unavailable.");

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println("Camera initialized successfully.");

    // Allow the sensor to stabilize.
    delay(1000);
}

// ======================================================
// SEND JPEG FRAME OVER UDP
// ======================================================

bool sendJpegUdp(
    const uint8_t* jpegData,
    uint32_t jpegLength
)
{
    if (jpegData == nullptr || jpegLength == 0)
    {
        Serial.println("Cannot send an empty JPEG.");
        return false;
    }

    const uint32_t frameId = frameCounter++;

    for (
        uint32_t offset = 0;
        offset < jpegLength;
        offset += UDP_CHUNK_SIZE
    )
    {
        // Process MQTT messages between UDP packets.
        //
        // This allows STREAM|0 to stop a large frame before
        // every UDP packet has been transmitted.
        mqttClient.loop();

        if (!cameraStreaming)
        {
            Serial.println(
                "Frame transmission interrupted by STREAM|0."
            );

            return false;
        }

        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println(
                "Frame transmission stopped: Wi-Fi disconnected."
            );

            return false;
        }

        if (!mqttClient.connected())
        {
            Serial.println(
                "Frame transmission stopped: MQTT disconnected."
            );

            return false;
        }

        const uint32_t remaining = jpegLength - offset;

        const uint16_t chunkLength =
            remaining > UDP_CHUNK_SIZE
                ? UDP_CHUNK_SIZE
                : static_cast<uint16_t>(remaining);

        UdpJpegHeader header;

        header.magic = MAGIC;
        header.frame_id = frameId;
        header.total_len = jpegLength;
        header.offset = offset;
        header.chunk_len = chunkLength;

        if (!udp.beginPacket(SERVER_IP, SERVER_PORT))
        {
            Serial.println("UDP beginPacket failed.");
            return false;
        }

        udp.write(
            reinterpret_cast<const uint8_t*>(&header),
            sizeof(header)
        );

        udp.write(
            jpegData + offset,
            chunkLength
        );

        if (!udp.endPacket())
        {
            Serial.println("UDP endPacket failed.");
            return false;
        }

        delayMicroseconds(UDP_PACKET_DELAY_US);
    }

    return true;
}

// ======================================================
// CAPTURE AND SEND ONE FRAME
// ======================================================

void captureAndSendFrame()
{
    camera_fb_t* frameBuffer = esp_camera_fb_get();

    if (frameBuffer == nullptr)
    {
        Serial.println("Camera capture failed.");
        return;
    }

    if (frameBuffer->format != PIXFORMAT_JPEG)
    {
        Serial.println("Captured frame is not JPEG.");

        esp_camera_fb_return(frameBuffer);
        return;
    }

    const size_t frameLength = frameBuffer->len;

    const bool sentSuccessfully = sendJpegUdp(
        frameBuffer->buf,
        static_cast<uint32_t>(frameLength)
    );

    // Always return the camera buffer.
    esp_camera_fb_return(frameBuffer);

    if (sentSuccessfully)
    {
        Serial.printf(
            "Sent frame %lu, size: %u bytes, interval: %lu ms\n",
            static_cast<unsigned long>(frameCounter - 1),
            static_cast<unsigned int>(frameLength),
            static_cast<unsigned long>(FRAME_INTERVAL_MS)
        );
    }
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println(" ESP32-S3 MQTT-Controlled Camera Sender");
    Serial.println("========================================");

    // Streaming must remain off until CAM|1 is received.
    cameraStreaming = false;

    // Default frame interval.
    FRAME_INTERVAL_MS = 200;

    // Connect to enterprise Wi-Fi.
    connectEnterpriseWiFi();

    // Initialize the camera.
    initCamera();

    // Start UDP with an automatically selected local port.
    if (!udp.begin(0))
    {
        Serial.println("UDP initialization failed.");
    }
    else
    {
        Serial.println("UDP initialized successfully.");
    }

    // Configure MQTT.
    mqttClient.setServer(
        MQTT_SERVER,
        MQTT_PORT
    );

    mqttClient.setCallback(
        mqttCallback
    );

    mqttClient.setBufferSize(1024);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(5);

    // Initial MQTT connection.
    connectMQTT();

    Serial.println();
    Serial.println("========================================");
    Serial.println("SYSTEM READY");
    Serial.println("========================================");

    Serial.print("UDP camera receiver: ");
    Serial.print(SERVER_IP);
    Serial.print(":");
    Serial.println(SERVER_PORT);

    Serial.print("MQTT control topic: ");
    Serial.println(MQTT_TOPIC);

    Serial.print("Default frame interval: ");
    Serial.print(FRAME_INTERVAL_MS);
    Serial.println(" ms");

    Serial.print("Approximate default FPS: ");
    Serial.println(
        1000.0f / static_cast<float>(FRAME_INTERVAL_MS),
        2
    );

    Serial.println();
    Serial.println("Camera streaming is currently OFF.");
    Serial.println();
    Serial.println("MQTT commands:");
    Serial.println("STREAM|1          Start streaming");
    Serial.println("STREAM|0          Stop streaming");
    Serial.println("FRAMERATE|x    Set frame interval");
    Serial.println();
    Serial.println("Example: FRAMERATE|200");
    Serial.println("========================================");
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop()
{
    // --------------------------------------------------
    // Maintain Wi-Fi connection
    // --------------------------------------------------

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Wi-Fi disconnected.");

        // Require a new CAM|1 after reconnecting.
        cameraStreaming = false;

        mqttClient.disconnect();

        connectEnterpriseWiFi();

        lastMqttReconnectAttempt = 0;
    }

    // --------------------------------------------------
    // Maintain MQTT connection
    // --------------------------------------------------

    if (!mqttClient.connected())
    {
        const uint32_t currentTime = millis();

        if (
            lastMqttReconnectAttempt == 0 ||
            currentTime - lastMqttReconnectAttempt >=
                MQTT_RETRY_INTERVAL_MS
        )
        {
            lastMqttReconnectAttempt = currentTime;

            if (connectMQTT())
            {
                lastMqttReconnectAttempt = 0;
            }
        }
    }
    else
    {
        // Process incoming MQTT commands.
        mqttClient.loop();
    }

    // --------------------------------------------------
    // Capture and transmit camera frames
    // --------------------------------------------------

    if (
        cameraStreaming &&
        mqttClient.connected() &&
        WiFi.status() == WL_CONNECTED
    )
    {
        const uint32_t currentTime = millis();

        // Unsigned subtraction allows this comparison to
        // continue working after millis() overflows.
        if (
            lastFrameTime == 0 ||
            currentTime - lastFrameTime >= FRAME_INTERVAL_MS
        )
        {
            lastFrameTime = currentTime;
            captureAndSendFrame();
        }
    }

    delay(2);
}
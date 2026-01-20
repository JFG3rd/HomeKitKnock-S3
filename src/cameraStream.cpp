/*
 * Project: HomeKitKnock-S3
 * File: src/cameraStream.cpp
 * Author: Jesse Greene
 */


#include "cameraStream.h"
#ifdef CAMERA
#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "logger.h"
#include "config.h"
#include "audio.h"

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *kStreamContentType = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *kStreamBoundary = "\r\n--" PART_BOUNDARY "\r\n";
static const char *kStreamPartHeader = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static const uint16_t kAudioStreamChannels = 1;
static const uint32_t kAudioStreamDataBytes = 0x7fffffff;
static const size_t kAudioSamplesPerChunk = 512;

static const uint8_t kStreamPort = 81;
static const uint8_t kClientSlots = 4;

// Dedicated MJPEG server to allow multiple concurrent clients.
static WiFiServer streamServer(kStreamPort);
static bool stream_server_running = false;
static bool stream_server_task_active = false;
static uint8_t stream_max_clients = 2;

struct StreamClientSlot {
    bool active = false;
    String ip = "";
    uint32_t start_ms = 0;
};

static StreamClientSlot stream_clients[kClientSlots];
static StreamClientSlot audio_clients[kClientSlots];
static uint32_t stream_last_frame_ms = 0;

struct StreamTaskArgs {
    WiFiClient *client = nullptr;
};

enum class StreamRequestType {
    kUnknown,
    kVideo,
    kAudio
};

// Forward declarations
static void stream_server_task(void *pvParameters);
static void stream_client_task(void *pvParameters);

static void sendStreamHeaders(WiFiClient &client) {
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Access-Control-Allow-Origin: *\r\n");
    client.print("Content-Type: ");
    client.print(kStreamContentType);
    client.print("\r\n");
    client.print("Connection: close\r\n\r\n");
}

static void writeWavStreamHeader(WiFiClient &client,
                                 uint32_t sampleRate,
                                 uint16_t bitsPerSample,
                                 uint16_t channels) {
    uint32_t dataBytes = kAudioStreamDataBytes;
    uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    uint16_t blockAlign = channels * bitsPerSample / 8;
    uint32_t chunkSize = 36 + dataBytes;

    client.write(reinterpret_cast<const uint8_t *>("RIFF"), 4);
    client.write(reinterpret_cast<const uint8_t *>(&chunkSize), 4);
    client.write(reinterpret_cast<const uint8_t *>("WAVE"), 4);
    client.write(reinterpret_cast<const uint8_t *>("fmt "), 4);
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1;
    client.write(reinterpret_cast<const uint8_t *>(&subchunk1Size), 4);
    client.write(reinterpret_cast<const uint8_t *>(&audioFormat), 2);
    client.write(reinterpret_cast<const uint8_t *>(&channels), 2);
    client.write(reinterpret_cast<const uint8_t *>(&sampleRate), 4);
    client.write(reinterpret_cast<const uint8_t *>(&byteRate), 4);
    client.write(reinterpret_cast<const uint8_t *>(&blockAlign), 2);
    client.write(reinterpret_cast<const uint8_t *>(&bitsPerSample), 2);
    client.write(reinterpret_cast<const uint8_t *>("data"), 4);
    client.write(reinterpret_cast<const uint8_t *>(&dataBytes), 4);
}

static void sendAudioHeaders(WiFiClient &client) {
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Access-Control-Allow-Origin: *\r\n");
    client.print("Cache-Control: no-store\r\n");
    client.print("Content-Type: audio/wav\r\n");
    client.print("Connection: close\r\n\r\n");
    writeWavStreamHeader(client, AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_BITS, kAudioStreamChannels);
}

static StreamRequestType parseStreamRequest(WiFiClient &client) {
    client.setTimeout(2);
    String requestLine = client.readStringUntil('\n');
    requestLine.trim();
    bool isStream = requestLine.startsWith("GET /stream");
    bool isAudio = requestLine.startsWith("GET /audio");

    // Drain the remaining HTTP headers so the socket is ready for streaming.
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line.length() == 0 || line == "\r") {
            break;
        }
    }

    if (isStream) {
        return StreamRequestType::kVideo;
    }
    if (isAudio) {
        return StreamRequestType::kAudio;
    }
    return StreamRequestType::kUnknown;
}

static uint8_t countActiveVideoClients() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < kClientSlots; i++) {
        if (stream_clients[i].active) {
            count++;
        }
    }
    return count;
}

static uint8_t countActiveAudioClients() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < kClientSlots; i++) {
        if (audio_clients[i].active) {
            count++;
        }
    }
    return count;
}

static uint8_t countActiveTotalClients() {
    return countActiveVideoClients() + countActiveAudioClients();
}

static int findFreeVideoSlot() {
    for (uint8_t i = 0; i < kClientSlots; i++) {
        if (!stream_clients[i].active) {
            return i;
        }
    }
    return -1;
}

static int findFreeAudioSlot() {
    for (uint8_t i = 0; i < kClientSlots; i++) {
        if (!audio_clients[i].active) {
            return i;
        }
    }
    return -1;
}

static void stream_client_task(void *pvParameters) {
    StreamTaskArgs *args = static_cast<StreamTaskArgs *>(pvParameters);
    if (!args || !args->client) {
        delete args;
        vTaskDelete(NULL);
        return;
    }

    WiFiClient *client = args->client;
    StreamRequestType requestType = parseStreamRequest(*client);

    if (requestType == StreamRequestType::kVideo) {
        int slot = findFreeVideoSlot();
        if (slot < 0) {
            client->print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
            client->stop();
            delete client;
            delete args;
            vTaskDelete(NULL);
            return;
        }

        stream_clients[slot].active = true;
        stream_clients[slot].ip = client->remoteIP().toString();
        stream_clients[slot].start_ms = millis();
        stream_last_frame_ms = stream_clients[slot].start_ms;

        sendStreamHeaders(*client);

        while (stream_server_running && client->connected()) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                Serial.println("Camera capture failed");
                vTaskDelay(10 / portTICK_PERIOD_MS);
                continue;
            }

            client->write(reinterpret_cast<const uint8_t *>(kStreamBoundary), strlen(kStreamBoundary));

            char part_buf[64];
            size_t hlen = snprintf(part_buf, sizeof(part_buf), kStreamPartHeader, fb->len);
            client->write(reinterpret_cast<const uint8_t *>(part_buf), hlen);

            size_t written = client->write(fb->buf, fb->len);
            stream_last_frame_ms = millis();

            esp_camera_fb_return(fb);
            if (written != fb->len) {
                break;
            }

            // Yield to avoid starving networking tasks and triggering the watchdog.
            vTaskDelay(1);
        }

        client->stop();
        logEvent(LOG_INFO, "📴 MJPEG client disconnected: " + stream_clients[slot].ip);
        delete client;

        stream_clients[slot].active = false;
        stream_clients[slot].ip = "";
        stream_clients[slot].start_ms = 0;
    } else if (requestType == StreamRequestType::kAudio) {
        if (!isMicEnabled()) {
            client->print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\nMic disabled");
            client->stop();
            delete client;
            delete args;
            vTaskDelete(NULL);
            return;
        }

        int slot = findFreeAudioSlot();
        if (slot < 0) {
            client->print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
            client->stop();
            delete client;
            delete args;
            vTaskDelete(NULL);
            return;
        }

        audio_clients[slot].active = true;
        audio_clients[slot].ip = client->remoteIP().toString();
        audio_clients[slot].start_ms = millis();

        sendAudioHeaders(*client);

        int16_t samples[kAudioSamplesPerChunk];
        const size_t bytesPerChunk = sizeof(samples);

        while (stream_server_running && client->connected()) {
            if (!isMicEnabled()) {
                break;
            }
            bool ok = captureMicSamples(samples, kAudioSamplesPerChunk, 80);
            if (!ok) {
                memset(samples, 0, bytesPerChunk);
            }
            size_t written = client->write(reinterpret_cast<const uint8_t *>(samples), bytesPerChunk);
            if (written != bytesPerChunk) {
                break;
            }

            // Yield to keep Wi-Fi and RTSP tasks responsive.
            vTaskDelay(1);
        }

        client->stop();
        logEvent(LOG_INFO, "📴 HTTP audio client disconnected: " + audio_clients[slot].ip);
        delete client;

        audio_clients[slot].active = false;
        audio_clients[slot].ip = "";
        audio_clients[slot].start_ms = 0;
    } else {
        client->print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
        client->stop();
        delete client;
    }

    delete args;
    vTaskDelete(NULL);
}

void startCameraStreamServer() {
    if (stream_server_running || stream_server_task_active) {
        return;
    }

    Serial.println("Setting up camera stream server...");
    stream_server_task_active = true;
    if (xTaskCreatePinnedToCore(
            stream_server_task,
            "stream_server",
            8192,
            NULL,
            1,
            NULL,
            STREAM_TASK_CORE) != pdPASS) {
        stream_server_task_active = false;
        Serial.println("❌ Failed to create camera stream server task");
        return;
    }
    Serial.println("Camera stream server task created");
}

static void stream_server_task(void *pvParameters) {
    // Give the system time to fully initialize networking.
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    streamServer.begin();
    stream_server_running = true;
    stream_server_task_active = false;

    Serial.printf("Starting stream server on port: %d\n", kStreamPort);
    Serial.println("Camera stream server started successfully");

    while (stream_server_running) {
        WiFiClient client = streamServer.available();
        if (client) {
            uint8_t activeClients = countActiveTotalClients();
            if (activeClients >= stream_max_clients) {
                client.stop();
            } else {
                WiFiClient *client_ptr = new WiFiClient(client);
                if (!client_ptr) {
                    client.stop();
                } else {
                    client_ptr->setNoDelay(true);
                    StreamTaskArgs *args = new StreamTaskArgs();
                    if (!args) {
                        client_ptr->stop();
                        delete client_ptr;
                    } else {
                        args->client = client_ptr;
                        if (xTaskCreatePinnedToCore(
                                stream_client_task,
                                "stream_client",
                                8192,
                                args,
                                1,
                                NULL,
                                STREAM_TASK_CORE) != pdPASS) {
                            client_ptr->stop();
                            delete client_ptr;
                            delete args;
                        }
                    }
                }
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    streamServer.stop();
    stream_server_running = false;
    vTaskDelete(NULL);
}

void stopCameraStreamServer() {
    if (!stream_server_running) {
        return;
    }

    stream_server_running = false;
    Serial.println("🛑 Camera stream server stopped");
}

bool isCameraStreamServerRunning() {
    return stream_server_running;
}

void setCameraStreamMaxClients(uint8_t maxClients) {
    if (maxClients == 0) {
        maxClients = 1;
    }
    if (maxClients > kClientSlots) {
        maxClients = kClientSlots;
    }
    stream_max_clients = maxClients;
}

bool getCameraStreamClientInfo(String &clientIp,
                               uint32_t &clientCount,
                               uint32_t &connectedMs,
                               uint32_t &lastFrameAgeMs,
                               String &clientsJson) {
    clientCount = countActiveVideoClients();
    uint32_t now_ms = millis();
    clientsJson = "[";
    bool first = true;
    clientIp = "";
    connectedMs = 0;

    for (uint8_t i = 0; i < kClientSlots; i++) {
        if (!stream_clients[i].active) {
            continue;
        }
        if (!first) {
            clientsJson += ",";
        }
        clientsJson += "\"" + stream_clients[i].ip + "\"";
        first = false;

        if (clientIp.isEmpty()) {
            clientIp = stream_clients[i].ip;
            connectedMs = now_ms - stream_clients[i].start_ms;
        }
    }
    clientsJson += "]";

    if (stream_last_frame_ms > 0) {
        lastFrameAgeMs = now_ms - stream_last_frame_ms;
    } else {
        lastFrameAgeMs = 0;
    }

    return clientCount > 0;
}

uint32_t getCameraStreamAudioClientCount() {
    return countActiveAudioClients();
}

#endif // CAMERA

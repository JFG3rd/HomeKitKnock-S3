/*
 * Project: HomeKitKnock-S3
 * File: src/cameraStream.cpp
 * Author: Jesse Greene
 */


#include "cameraStream.h"
#ifdef CAMERA
#include <esp_http_server.h>
#include <esp_camera.h>
#include "Arduino.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Global handle for the streaming HTTP server.
static httpd_handle_t stream_httpd = NULL;
static bool stream_server_running = false;

static esp_err_t stream_handler(httpd_req_t *req)
{
    // MJPEG multipart stream loop. Sends boundary + JPEG frames indefinitely.
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[64];

    // int64_t last_frame = esp_timer_get_time();

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        }
        else
        {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK)
        {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf)
        {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK)
        {
            break;
        }
        // Yield to avoid starving async TCP tasks and triggering the watchdog.
        vTaskDelay(1);
        // int64_t fr_end = esp_timer_get_time();

/*
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        Serial.printf("MJPG: %uB %ums (%.1ffps), free heap: %d\n",
                      (uint32_t)(_jpg_buf_len),
                      (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
                      ESP.getFreeHeap());
*/
    }

    return res;
}

// Forward declarations
static void stream_server_task(void* pvParameters);

void startCameraStreamServer()
{
    // Start a dedicated httpd task so streaming doesn't block AsyncWebServer.
    if (stream_server_running) {
        return;
    }

    Serial.println("Setting up camera stream server...");
    // Spawn a low-priority task to start the server after networking settles.
    xTaskCreate(
        stream_server_task,   // Task function
        "stream_server",      // Name of task
        8192,                // Stack size
        NULL,                // Parameters
        1,                   // Priority (1 = low)
        NULL                 // Task handle
    );
    stream_server_running = true;
    Serial.println("Camera stream server task created");
}

// This task runs separately to avoid conflicts with AsyncWebServer.
static void stream_server_task(void* pvParameters)
{
    // Give the system time to fully initialize networking.
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    // Configure httpd server on port 81 for MJPEG streaming.
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // Use port 81 for streaming.
    config.server_port = 81;
    config.ctrl_port = 32769;
    
    // Configure for better stability.
    config.stack_size = 8192;
    // Run on the opposite core from AsyncTCP to reduce contention.
    config.core_id = 1;
    
    Serial.printf("Starting stream server on port: %d\n", config.server_port);
    
    // Start the server.
    esp_err_t ret = httpd_start(&stream_httpd, &config);
    if (ret == ESP_OK) {
        // Define stream URI handler.
        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL
        };
        
        // Register URI handler.
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        Serial.println("Camera stream server started successfully");
    } else {
        Serial.printf("Camera stream server failed to start: %d\n", ret);
    }
    
    // Task can be deleted after setup.
    vTaskDelete(NULL);
}

#endif // CAMERA

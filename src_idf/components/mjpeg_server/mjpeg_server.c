/**
 * MJPEG Streaming Server Implementation
 *
 * Raw lwIP socket server on port 81 for MJPEG streaming.
 * Matches the Arduino version's boundary and behavior so
 * live.html works unchanged.
 */

#include "mjpeg_server.h"
#include "camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "mjpeg";

#define MJPEG_PORT          81
#define MAX_CLIENTS         2
#define STREAM_TASK_STACK   4096
#define SERVER_TASK_STACK   4096
#define STREAM_CORE         1

// Boundary must match Arduino version for live.html compatibility
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *STREAM_CONTENT_TYPE =
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Type: multipart/x-mixed-replace;boundary=" PART_BOUNDARY "\r\n"
    "Connection: close\r\n\r\n";

static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_FMT = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static volatile bool server_running = false;
static volatile uint8_t active_clients = 0;
static int server_sock = -1;
static TaskHandle_t server_task_handle = NULL;

/**
 * Send all bytes, handling partial writes
 */
static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        int sent = send(sock, p, remaining, 0);
        if (sent < 0) {
            return -1;
        }
        p += sent;
        remaining -= sent;
    }
    return (int)len;
}

/**
 * Drain HTTP request headers from client socket
 * Reads until we see the blank line ending the HTTP request
 */
static void drain_http_request(int sock) {
    char buf[256];
    // Set a short timeout for reading headers
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int total = 0;
    while (total < 2048) {  // Safety limit
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        total += n;
        // Check if we've received the end of HTTP headers
        if (strstr(buf, "\r\n\r\n")) break;
    }
}

/**
 * Per-client streaming task
 */
static void stream_client_task(void *pvParameters) {
    int client_sock = (int)(intptr_t)pvParameters;
    active_clients++;

    ESP_LOGI(TAG, "Client connected (active: %d)", active_clients);

    // Drain the incoming HTTP request headers
    drain_http_request(client_sock);

    // Set send timeout
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Send multipart HTTP response headers
    if (send_all(client_sock, STREAM_CONTENT_TYPE, strlen(STREAM_CONTENT_TYPE)) < 0) {
        ESP_LOGW(TAG, "Failed to send headers");
        goto cleanup;
    }

    // Streaming loop
    while (server_running) {
        camera_fb_t *fb = camera_capture();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Send boundary
        if (send_all(client_sock, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) < 0) {
            camera_return_fb(fb);
            break;
        }

        // Send part header with content length
        char part_header[64];
        int hlen = snprintf(part_header, sizeof(part_header), STREAM_PART_FMT, fb->len);
        if (send_all(client_sock, part_header, hlen) < 0) {
            camera_return_fb(fb);
            break;
        }

        // Send JPEG frame data
        int ret = send_all(client_sock, fb->buf, fb->len);
        camera_return_fb(fb);

        if (ret < 0) {
            break;
        }

        // Yield to avoid starving other tasks
        vTaskDelay(pdMS_TO_TICKS(1));
    }

cleanup:
    close(client_sock);
    active_clients--;
    ESP_LOGI(TAG, "Client disconnected (active: %d)", active_clients);
    vTaskDelete(NULL);
}

/**
 * Server listener task - accepts connections and spawns client tasks
 */
static void server_task(void *pvParameters) {
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MJPEG_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(server_sock);
        server_sock = -1;
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_sock, 2) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(server_sock);
        server_sock = -1;
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MJPEG server listening on port %d", MJPEG_PORT);

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        // Set accept timeout so we can check server_running periodically
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Timeout, check server_running and loop
            }
            if (server_running) {
                ESP_LOGW(TAG, "Accept failed: errno %d", errno);
            }
            continue;
        }

        // Check if we have room for another client
        if (active_clients >= MAX_CLIENTS) {
            ESP_LOGW(TAG, "Max clients reached, rejecting connection");
            const char *busy = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
            send(client_sock, busy, strlen(busy), 0);
            close(client_sock);
            continue;
        }

        // Log client IP
        char ip_str[16];
        inet_ntoa_r(client_addr.sin_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "New client from %s", ip_str);

        // Spawn per-client streaming task on core 1
        char task_name[24];
        snprintf(task_name, sizeof(task_name), "mjpeg_cli_%d", client_sock);

        BaseType_t ret = xTaskCreatePinnedToCore(
            stream_client_task,
            task_name,
            STREAM_TASK_STACK,
            (void *)(intptr_t)client_sock,
            1,  // Priority
            NULL,
            STREAM_CORE
        );

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            close(client_sock);
        }
    }

    // Cleanup
    close(server_sock);
    server_sock = -1;
    ESP_LOGI(TAG, "MJPEG server stopped");
    vTaskDelete(NULL);
}

esp_err_t mjpeg_server_start(void) {
    if (server_running) {
        ESP_LOGW(TAG, "MJPEG server already running");
        return ESP_OK;
    }

    if (!camera_is_ready()) {
        ESP_LOGE(TAG, "Cannot start MJPEG server: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }

    server_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        server_task,
        "mjpeg_server",
        SERVER_TASK_STACK,
        NULL,
        1,  // Priority
        &server_task_handle,
        STREAM_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create server task");
        server_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mjpeg_server_stop(void) {
    if (!server_running) return;

    ESP_LOGI(TAG, "Stopping MJPEG server...");
    server_running = false;

    // Close the server socket to unblock accept()
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }

    // Wait for clients to disconnect
    int timeout = 50;  // 5 seconds max
    while (active_clients > 0 && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
}

uint8_t mjpeg_server_client_count(void) {
    return active_clients;
}

bool mjpeg_server_is_running(void) {
    return server_running;
}

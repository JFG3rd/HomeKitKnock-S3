/**
 * DNS Server Component Implementation
 *
 * Simple DNS server that responds to all queries with the AP's IP address.
 * This enables captive portal functionality by redirecting all DNS lookups
 * to the ESP32's access point.
 */

#include "dns_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "dns_server";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_TASK_STACK_SIZE 4096
#define DNS_TASK_PRIORITY 5

// AP IP address to return for all queries
#define AP_IP_ADDR_0 192
#define AP_IP_ADDR_1 168
#define AP_IP_ADDR_2 4
#define AP_IP_ADDR_3 1

// DNS header structure
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

// DNS answer structure for A record
typedef struct __attribute__((packed)) {
    uint16_t name;      // Pointer to name in question (0xC00C)
    uint16_t type;      // Type A (1)
    uint16_t class;     // Class IN (1)
    uint32_t ttl;       // Time to live
    uint16_t rdlength;  // Data length (4 for IPv4)
    uint8_t rdata[4];   // IP address
} dns_answer_t;

static int dns_socket = -1;
static TaskHandle_t dns_task_handle = NULL;
static bool running = false;

/**
 * Parse DNS query and build response
 * Returns response length, or -1 on error
 */
static int dns_build_response(uint8_t *request, int request_len, uint8_t *response) {
    if (request_len < (int)sizeof(dns_header_t)) {
        return -1;
    }

    // Copy the request header
    dns_header_t *resp_header = (dns_header_t *)response;

    // Copy header and modify for response
    memcpy(response, request, request_len);

    // Set response flags: QR=1 (response), AA=1 (authoritative), RD=1 (recursion desired)
    resp_header->flags = htons(0x8400);  // Standard query response, no error

    // Set answer count to 1
    resp_header->ancount = htons(1);

    // Find the end of the question section
    // Question format: QNAME (labels) + QTYPE (2) + QCLASS (2)
    int question_offset = sizeof(dns_header_t);
    while (question_offset < request_len && request[question_offset] != 0) {
        question_offset += request[question_offset] + 1;
    }
    question_offset++;  // Skip null terminator
    question_offset += 4;  // Skip QTYPE and QCLASS

    if (question_offset > request_len) {
        return -1;
    }

    // Build answer section
    dns_answer_t *answer = (dns_answer_t *)(response + question_offset);
    answer->name = htons(0xC00C);  // Pointer to name at offset 12
    answer->type = htons(1);       // Type A
    answer->class = htons(1);      // Class IN
    answer->ttl = htonl(60);       // 60 seconds TTL
    answer->rdlength = htons(4);   // IPv4 address length
    answer->rdata[0] = AP_IP_ADDR_0;
    answer->rdata[1] = AP_IP_ADDR_1;
    answer->rdata[2] = AP_IP_ADDR_2;
    answer->rdata[3] = AP_IP_ADDR_3;

    return question_offset + sizeof(dns_answer_t);
}

/**
 * DNS server task
 */
static void dns_server_task(void *pvParameters) {
    uint8_t request[DNS_MAX_LEN];
    uint8_t response[DNS_MAX_LEN];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    ESP_LOGI(TAG, "DNS server task started");

    while (running) {
        client_addr_len = sizeof(client_addr);

        // Receive DNS query
        int len = recvfrom(dns_socket, request, sizeof(request), 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: %d", errno);
            break;
        }

        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        // Build response
        int response_len = dns_build_response(request, len, response);
        if (response_len < 0) {
            ESP_LOGW(TAG, "Failed to build DNS response");
            continue;
        }

        // Send response
        int sent = sendto(dns_socket, response, response_len, 0,
                         (struct sockaddr *)&client_addr, client_addr_len);

        if (sent < 0) {
            ESP_LOGW(TAG, "sendto failed: %d", errno);
        } else {
            ESP_LOGD(TAG, "DNS response sent (%d bytes)", sent);
        }
    }

    ESP_LOGI(TAG, "DNS server task stopped");
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void) {
    if (running) {
        ESP_LOGW(TAG, "DNS server already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting DNS server on port %d", DNS_PORT);

    // Create UDP socket
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return ESP_FAIL;
    }

    // Set socket options
    int opt = 1;
    setsockopt(dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set receive timeout
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Bind to DNS port
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
        close(dns_socket);
        dns_socket = -1;
        return ESP_FAIL;
    }

    running = true;

    // Create DNS server task
    BaseType_t ret = xTaskCreate(dns_server_task, "dns_server",
                                  DNS_TASK_STACK_SIZE, NULL,
                                  DNS_TASK_PRIORITY, &dns_task_handle);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS task");
        running = false;
        close(dns_socket);
        dns_socket = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DNS server started - redirecting all queries to %d.%d.%d.%d",
             AP_IP_ADDR_0, AP_IP_ADDR_1, AP_IP_ADDR_2, AP_IP_ADDR_3);

    return ESP_OK;
}

esp_err_t dns_server_stop(void) {
    if (!running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping DNS server");
    running = false;

    // Close socket to unblock recvfrom
    if (dns_socket >= 0) {
        close(dns_socket);
        dns_socket = -1;
    }

    // Wait for task to finish
    if (dns_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        dns_task_handle = NULL;
    }

    ESP_LOGI(TAG, "DNS server stopped");
    return ESP_OK;
}

bool dns_server_is_running(void) {
    return running;
}

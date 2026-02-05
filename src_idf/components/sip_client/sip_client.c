/*
 * Project: HomeKitKnock-S3
 * File: src_idf/components/sip_client/sip_client.c
 * Purpose: SIP client for Fritz!Box integration (ESP-IDF)
 * Author: Jesse Greene
 */

#include "sip_client.h"
#include "nvs_manager.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "mbedtls/md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sip";

// SIP constants
#define SIP_DOMAIN          "fritz.box"
#define SIP_PROXY           "fritz.box"
#define SIP_PORT            5060
#define LOCAL_SIP_PORT      5062
#define SIP_RTP_PORT        40000
#define SIP_MSG_BUF_SIZE    2048
#define SIP_SMALL_BUF_SIZE  256

// Timing constants (milliseconds)
#define REGISTER_INTERVAL_MS    60000
#define SIP_RESPONSE_TIMEOUT_MS 2000
#define SIP_RING_DURATION_MS    30000
#define SIP_CANCEL_WAIT_MS      3000
#define SIP_IN_CALL_HOLD_MS     60000

// NVS namespace and keys
#define NVS_SIP_NAMESPACE   "sip"
#define NVS_KEY_USER        "sip_user"
#define NVS_KEY_PASSWORD    "sip_password"
#define NVS_KEY_DISPLAYNAME "sip_displayname"
#define NVS_KEY_TARGET      "sip_target"
#define NVS_KEY_ENABLED     "sip_enabled"
#define NVS_KEY_VERBOSE     "sip_verbose"

// Authentication challenge
typedef struct {
    char realm[128];
    char nonce[128];
    char algorithm[16];
    char qop[32];
    char opaque[128];
    bool is_proxy;
    bool valid;
} auth_challenge_t;

// SIP media info from SDP
typedef struct {
    uint32_t remote_ip;
    uint16_t remote_port;
    bool has_pcmu;
    bool has_pcma;
    uint8_t preferred_audio_payload;
    uint8_t dtmf_payload;
    bool remote_sends;
    bool remote_receives;
} sip_media_info_t;

// Pending INVITE state
typedef struct {
    bool active;
    bool auth_sent;
    bool can_cancel;
    bool answered;
    bool ack_sent;
    bool bye_sent;
    bool cancel_sent;
    char call_id[64];
    char from_tag[32];
    char to_tag[32];
    uint32_t cseq;
    char branch[64];
    char target[128];
    char remote_target[256];
    uint32_t invite_start_ms;
    uint32_t answered_ms;
    uint32_t cancel_start_ms;
    bool media_ready;
    sip_media_info_t media;
    sip_config_t config;
} pending_invite_t;

// Active call session
typedef struct {
    bool active;
    bool inbound;
    bool awaiting_ack;
    bool acked;
    bool bye_sent;
    char call_id[64];
    char local_tag[32];
    char remote_tag[32];
    char remote_contact[256];
    char remote_uri[256];
    char request_uri[256];
    uint32_t local_cseq;
    uint32_t remote_cseq;
    uint32_t sip_remote_ip;
    uint16_t sip_remote_port;
    uint32_t rtp_remote_ip;
    uint16_t rtp_remote_port;
    uint8_t audio_payload;
    uint8_t dtmf_payload;
    bool remote_sends;
    bool remote_receives;
    bool local_sends;
    bool local_receives;
    uint32_t start_ms;
    uint32_t last_rtp_send_ms;
    uint32_t last_rtp_recv_ms;
    uint16_t rtp_seq;
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;
    sip_config_t config;
} sip_call_session_t;

// Module state
static int sip_socket = -1;
static int rtp_socket = -1;
static bool sip_initialized = false;
static auth_challenge_t last_auth_challenge;
static uint32_t nonce_count = 1;
static pending_invite_t pending_invite;
static sip_call_session_t sip_call;
static sip_ring_tick_cb_t ring_tick_callback = NULL;
static sip_dtmf_cb_t dtmf_callback = NULL;
static uint32_t last_register_time = 0;
static uint32_t last_register_attempt_ms = 0;
static uint32_t last_register_ok_ms = 0;
static bool last_register_successful = false;
static int last_register_status = 0;
static uint32_t last_sip_net_warn_ms = 0;
static struct sockaddr_in last_remote_addr;

// Static buffers to avoid stack overflow (SIP is single-threaded)
static char sip_msg_buf[SIP_MSG_BUF_SIZE];      // Main SIP message buffer
static char sip_msg_buf2[SIP_MSG_BUF_SIZE];     // Secondary buffer for ACK during auth
static char sip_sdp_buf[512];                    // SDP body buffer
static char sip_auth_buf[512];                   // Auth calculation buffer
static char sip_local_ip[16];                    // Cached local IP string
static char sip_target_buf[96];                  // Target URI buffer
static char sip_request_uri[128];                // Request URI buffer

// Cached proxy IP to avoid gethostbyname on every send
static uint32_t cached_proxy_ip = 0;

// Deferred ring request flag (set from HTTP handler, executed in main loop)
static volatile bool ring_requested = false;
static uint32_t cached_proxy_time = 0;

// Verbose logging flag (shows full SIP message content)
static bool verbose_logging = false;
#define PROXY_CACHE_TIMEOUT_MS 60000

// Forward declarations
static bool sip_send(const char *msg, size_t len);
static bool sip_send_to(uint32_t ip, uint16_t port, const char *msg, size_t len);
static bool resolve_sip_proxy(uint32_t *ip);
static void load_verbose_logging_state(void);

// Get current time in milliseconds
static uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Get local IP address as 32-bit value
static uint32_t get_local_ip(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return 0;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.ip.addr;
}

// Get gateway IP address
static uint32_t get_gateway_ip(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return 0;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.gw.addr;
}

// Convert IP to string
static void ip_to_str(uint32_t ip, char *buf, size_t len) {
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned int)((ip >> 0) & 0xFF),
             (unsigned int)((ip >> 8) & 0xFF),
             (unsigned int)((ip >> 16) & 0xFF),
             (unsigned int)((ip >> 24) & 0xFF));
}

// Parse IP from string
static uint32_t ip_from_str(const char *str) {
    unsigned int a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        return (d << 24) | (c << 16) | (b << 8) | a;
    }
    return 0;
}

// MD5 hash helper
static void md5_hex(const char *input, char *output) {
    unsigned char digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);

    for (int i = 0; i < 16; i++) {
        sprintf(output + (i * 2), "%02x", digest[i]);
    }
    output[32] = '\0';
}

// Generate random tag
static void generate_tag(char *buf, size_t len) {
    snprintf(buf, len, "%08x", (unsigned int)esp_random());
}

// Generate random branch
static void generate_branch(char *buf, size_t len) {
    snprintf(buf, len, "z9hG4bK-%08x", (unsigned int)esp_random());
}

// Generate call ID
static void generate_call_id(char *buf, size_t len) {
    char ip_str[16];
    ip_to_str(get_local_ip(), ip_str, sizeof(ip_str));
    snprintf(buf, len, "%08x@%s", (unsigned int)esp_random(), ip_str);
}

// Check if network is ready for SIP
static bool is_sip_network_ready(void) {
    if (!sip_initialized || sip_socket < 0) {
        return false;
    }

    if (!wifi_manager_is_connected()) {
        if (millis() - last_sip_net_warn_ms > 10000) {
            ESP_LOGW(TAG, "SIP paused: WiFi not connected");
            last_sip_net_warn_ms = millis();
        }
        return false;
    }

    if (get_local_ip() == 0) {
        if (millis() - last_sip_net_warn_ms > 10000) {
            ESP_LOGW(TAG, "SIP paused: invalid local IP");
            last_sip_net_warn_ms = millis();
        }
        return false;
    }

    return true;
}

// Resolve SIP proxy with caching (gethostbyname uses too much stack)
static bool resolve_sip_proxy(uint32_t *ip) {
    uint32_t now = millis();

    // Use cached IP if still valid
    if (cached_proxy_ip != 0 && (now - cached_proxy_time) < PROXY_CACHE_TIMEOUT_MS) {
        *ip = cached_proxy_ip;
        return true;
    }

    // Try gateway IP first (no DNS needed, saves stack)
    uint32_t gateway = get_gateway_ip();
    if (gateway != 0) {
        cached_proxy_ip = gateway;
        cached_proxy_time = now;
        *ip = gateway;
        ESP_LOGD(TAG, "Using gateway as SIP proxy");
        return true;
    }

    // Fall back to DNS only if gateway unavailable
    // Note: gethostbyname uses significant stack space
    struct hostent *he = gethostbyname(SIP_PROXY);
    if (he && he->h_addr_list[0]) {
        cached_proxy_ip = *(uint32_t *)he->h_addr_list[0];
        cached_proxy_time = now;
        *ip = cached_proxy_ip;
        return true;
    }

    return false;
}

// Extract header value from SIP message
static bool extract_header(const char *msg, const char *header, char *value, size_t len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s:", header);

    const char *pos = strcasestr(msg, needle);
    if (!pos) return false;

    pos += strlen(needle);
    while (*pos == ' ') pos++;

    const char *end = strstr(pos, "\r\n");
    if (!end) end = pos + strlen(pos);

    size_t copy_len = end - pos;
    if (copy_len >= len) copy_len = len - 1;

    strncpy(value, pos, copy_len);
    value[copy_len] = '\0';

    // Trim trailing whitespace
    char *trim = value + strlen(value) - 1;
    while (trim >= value && (*trim == ' ' || *trim == '\t')) {
        *trim-- = '\0';
    }

    return true;
}

// Extract tag from header value
static bool extract_tag(const char *header_value, char *tag, size_t len) {
    const char *pos = strstr(header_value, "tag=");
    if (!pos) return false;

    pos += 4;
    const char *end = strchr(pos, ';');
    if (!end) end = strchr(pos, '>');
    if (!end) end = pos + strlen(pos);

    size_t copy_len = end - pos;
    if (copy_len >= len) copy_len = len - 1;

    strncpy(tag, pos, copy_len);
    tag[copy_len] = '\0';
    return true;
}

// Extract SIP URI from header
static bool extract_sip_uri(const char *header_value, char *uri, size_t len) {
    const char *start = strstr(header_value, "sip:");
    if (!start) return false;

    const char *end = strchr(start, '>');
    if (!end) end = strchr(start, ';');
    if (!end) end = start + strlen(start);

    size_t copy_len = end - start;
    if (copy_len >= len) copy_len = len - 1;

    strncpy(uri, start, copy_len);
    uri[copy_len] = '\0';
    return true;
}

// Extract Via branch
static bool extract_via_branch(const char *msg, char *branch, size_t len) {
    char via[256];
    if (!extract_header(msg, "Via", via, sizeof(via))) {
        return false;
    }

    const char *pos = strstr(via, "branch=");
    if (!pos) return false;

    pos += 7;
    const char *end = strchr(pos, ';');
    if (!end) end = pos + strlen(pos);

    size_t copy_len = end - pos;
    if (copy_len >= len) copy_len = len - 1;

    strncpy(branch, pos, copy_len);
    branch[copy_len] = '\0';
    return true;
}

// Get SIP status code from response
static int get_status_code(const char *response) {
    if (strncmp(response, "SIP/2.0 ", 8) != 0) return -1;
    return atoi(response + 8);
}

// Parse CSeq header
static bool parse_cseq(const char *msg, uint32_t *cseq, char *method, size_t method_len) {
    char cseq_line[64];
    if (!extract_header(msg, "CSeq", cseq_line, sizeof(cseq_line))) {
        return false;
    }

    char *space = strchr(cseq_line, ' ');
    if (!space) return false;

    *cseq = atoi(cseq_line);
    strncpy(method, space + 1, method_len - 1);
    method[method_len - 1] = '\0';
    return true;
}

// Extract Contact URI
static bool extract_contact_uri(const char *msg, char *uri, size_t len) {
    char contact[256];
    if (!extract_header(msg, "Contact", contact, sizeof(contact))) {
        if (!extract_header(msg, "m", contact, sizeof(contact))) {
            return false;
        }
    }
    return extract_sip_uri(contact, uri, len);
}

// Extract To tag
static bool extract_to_tag(const char *msg, char *tag, size_t len) {
    char to[256];
    if (!extract_header(msg, "To", to, sizeof(to))) {
        if (!extract_header(msg, "t", to, sizeof(to))) {
            return false;
        }
    }
    return extract_tag(to, tag, len);
}

// Extract SDP body
static const char *extract_sdp_body(const char *msg) {
    const char *body = strstr(msg, "\r\n\r\n");
    return body ? body + 4 : NULL;
}

// Parse SDP media info
static void parse_sdp_media(const char *sdp, uint32_t fallback_ip, sip_media_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->remote_ip = fallback_ip;
    info->preferred_audio_payload = 0xFF;
    info->dtmf_payload = 101;
    info->remote_sends = true;
    info->remote_receives = true;

    if (!sdp) return;

    const char *line = sdp;
    while (line && *line) {
        const char *next = strstr(line, "\n");
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        char line_buf[256];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        strncpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        // Remove \r if present
        char *cr = strchr(line_buf, '\r');
        if (cr) *cr = '\0';

        if (strncmp(line_buf, "c=", 2) == 0) {
            // Connection line: c=IN IP4 x.x.x.x
            const char *ip_start = strstr(line_buf, "IN IP4");
            if (ip_start) {
                ip_start += 6;
                while (*ip_start == ' ') ip_start++;
                info->remote_ip = ip_from_str(ip_start);
            }
        } else if (strncmp(line_buf, "m=audio", 7) == 0) {
            // Media line: m=audio PORT RTP/AVP 0 8 101
            int port;
            if (sscanf(line_buf, "m=audio %d", &port) == 1) {
                info->remote_port = port;
            }
            if (strstr(line_buf, " 0") || strstr(line_buf, " 0 ")) {
                info->has_pcmu = true;
                if (info->preferred_audio_payload == 0xFF) {
                    info->preferred_audio_payload = 0;
                }
            }
            if (strstr(line_buf, " 8") || strstr(line_buf, " 8 ")) {
                info->has_pcma = true;
                if (info->preferred_audio_payload == 0xFF) {
                    info->preferred_audio_payload = 8;
                }
            }
        } else if (strncmp(line_buf, "a=rtpmap:", 9) == 0) {
            int pt;
            char codec[32];
            if (sscanf(line_buf, "a=rtpmap:%d %31s", &pt, codec) == 2) {
                if (strcasestr(codec, "PCMU/8000")) {
                    info->has_pcmu = true;
                } else if (strcasestr(codec, "PCMA/8000")) {
                    info->has_pcma = true;
                } else if (strcasestr(codec, "telephone-event")) {
                    info->dtmf_payload = pt;
                }
            }
        } else if (strcmp(line_buf, "a=sendonly") == 0) {
            info->remote_sends = true;
            info->remote_receives = false;
        } else if (strcmp(line_buf, "a=recvonly") == 0) {
            info->remote_sends = false;
            info->remote_receives = true;
        } else if (strcmp(line_buf, "a=inactive") == 0) {
            info->remote_sends = false;
            info->remote_receives = false;
        } else if (strcmp(line_buf, "a=sendrecv") == 0) {
            info->remote_sends = true;
            info->remote_receives = true;
        }

        line = next ? next + 1 : NULL;
    }
}

// Parse authentication challenge from 401/407 response
static void parse_auth_challenge(const char *response, auth_challenge_t *challenge) {
    memset(challenge, 0, sizeof(*challenge));

    const char *auth_line = strcasestr(response, "WWW-Authenticate:");
    if (!auth_line) {
        auth_line = strcasestr(response, "Proxy-Authenticate:");
        if (auth_line) {
            challenge->is_proxy = true;
        }
    }
    if (!auth_line) return;

    const char *end = strstr(auth_line, "\r\n");
    if (!end) return;

    // Parse realm
    const char *realm_start = strstr(auth_line, "realm=\"");
    if (realm_start && realm_start < end) {
        realm_start += 7;
        const char *realm_end = strchr(realm_start, '"');
        if (realm_end && realm_end < end) {
            size_t len = realm_end - realm_start;
            if (len >= sizeof(challenge->realm)) len = sizeof(challenge->realm) - 1;
            strncpy(challenge->realm, realm_start, len);
        }
    }

    // Parse nonce
    const char *nonce_start = strstr(auth_line, "nonce=\"");
    if (nonce_start && nonce_start < end) {
        nonce_start += 7;
        const char *nonce_end = strchr(nonce_start, '"');
        if (nonce_end && nonce_end < end) {
            size_t len = nonce_end - nonce_start;
            if (len >= sizeof(challenge->nonce)) len = sizeof(challenge->nonce) - 1;
            strncpy(challenge->nonce, nonce_start, len);
        }
    }

    // Parse algorithm (default MD5)
    const char *algo_start = strstr(auth_line, "algorithm=");
    if (algo_start && algo_start < end) {
        algo_start += 10;
        if (*algo_start == '"') algo_start++;
        const char *algo_end = algo_start;
        while (algo_end < end && *algo_end != ',' && *algo_end != '"' && *algo_end != '\r') {
            algo_end++;
        }
        size_t len = algo_end - algo_start;
        if (len >= sizeof(challenge->algorithm)) len = sizeof(challenge->algorithm) - 1;
        strncpy(challenge->algorithm, algo_start, len);
    } else {
        strcpy(challenge->algorithm, "MD5");
    }

    // Parse qop
    const char *qop_start = strstr(auth_line, "qop=\"");
    if (qop_start && qop_start < end) {
        qop_start += 5;
        const char *qop_end = strchr(qop_start, '"');
        if (qop_end && qop_end < end) {
            size_t len = qop_end - qop_start;
            if (len >= sizeof(challenge->qop)) len = sizeof(challenge->qop) - 1;
            strncpy(challenge->qop, qop_start, len);
        }
    } else {
        qop_start = strstr(auth_line, "qop=");
        if (qop_start && qop_start < end) {
            qop_start += 4;
            const char *qop_end = qop_start;
            while (qop_end < end && *qop_end != ',' && *qop_end != '\r') {
                qop_end++;
            }
            size_t len = qop_end - qop_start;
            if (len >= sizeof(challenge->qop)) len = sizeof(challenge->qop) - 1;
            strncpy(challenge->qop, qop_start, len);
        }
    }

    // Parse opaque
    const char *opaque_start = strstr(auth_line, "opaque=\"");
    if (opaque_start && opaque_start < end) {
        opaque_start += 8;
        const char *opaque_end = strchr(opaque_start, '"');
        if (opaque_end && opaque_end < end) {
            size_t len = opaque_end - opaque_start;
            if (len >= sizeof(challenge->opaque)) len = sizeof(challenge->opaque) - 1;
            strncpy(challenge->opaque, opaque_start, len);
        }
    }

    challenge->valid = (challenge->realm[0] != '\0' && challenge->nonce[0] != '\0');
}

// Calculate digest authentication response
// Uses static sip_auth_buf to avoid stack overflow
static void calculate_digest_response(
    const char *username,
    const char *password,
    const char *method,
    const char *uri,
    const auth_challenge_t *challenge,
    char *nc_out,
    char *cnonce_out,
    char *response_out
) {
    char ha1[33];
    char ha2[33];

    // HA1 = MD5(username:realm:password) - use static buffer
    snprintf(sip_auth_buf, sizeof(sip_auth_buf), "%s:%s:%s", username, challenge->realm, password);
    md5_hex(sip_auth_buf, ha1);

    // HA2 = MD5(method:uri)
    snprintf(sip_auth_buf, sizeof(sip_auth_buf), "%s:%s", method, uri);
    md5_hex(sip_auth_buf, ha2);

    if (challenge->qop[0] == '\0') {
        // Without qop: response = MD5(HA1:nonce:HA2)
        snprintf(sip_auth_buf, sizeof(sip_auth_buf), "%s:%s:%s", ha1, challenge->nonce, ha2);
    } else {
        // With qop=auth: response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
        snprintf(nc_out, 9, "%08x", (unsigned int)nonce_count);
        snprintf(cnonce_out, 9, "%08x", (unsigned int)esp_random());
        snprintf(sip_auth_buf, sizeof(sip_auth_buf), "%s:%s:%s:%s:auth:%s",
                 ha1, challenge->nonce, nc_out, cnonce_out, ha2);
    }

    md5_hex(sip_auth_buf, response_out);
}

// Build Authorization header
static int build_auth_header(
    char *buf,
    size_t len,
    const char *username,
    const char *password,
    const char *method,
    const char *uri,
    const auth_challenge_t *challenge
) {
    char nc[16] = "";
    char cnonce[16] = "";
    char response[33];

    calculate_digest_response(username, password, method, uri, challenge, nc, cnonce, response);

    const char *header_name = challenge->is_proxy ? "Proxy-Authorization" : "Authorization";

    int written = snprintf(buf, len,
        "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
        header_name, username, challenge->realm, challenge->nonce, uri, response);

    if (challenge->algorithm[0]) {
        written += snprintf(buf + written, len - written, ", algorithm=%s", challenge->algorithm);
    }

    if (challenge->qop[0]) {
        written += snprintf(buf + written, len - written,
            ", qop=auth, nc=%s, cnonce=\"%s\"", nc, cnonce);
        nonce_count++;
    }

    if (challenge->opaque[0]) {
        written += snprintf(buf + written, len - written, ", opaque=\"%s\"", challenge->opaque);
    }

    written += snprintf(buf + written, len - written, "\r\n");

    return written;
}

// Build SIP REGISTER message
static int build_register(char *buf, size_t len, const sip_config_t *config,
                          const char *from_tag, const char *call_id, const char *branch,
                          uint32_t cseq, bool with_auth) {
    char local_ip[16];
    ip_to_str(get_local_ip(), local_ip, sizeof(local_ip));

    char uri[64];
    snprintf(uri, sizeof(uri), "sip:%s", SIP_DOMAIN);

    int written = snprintf(buf, len,
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n",
        uri,
        local_ip, LOCAL_SIP_PORT, branch,
        config->sip_displayname, config->sip_user, SIP_DOMAIN, from_tag,
        config->sip_user, SIP_DOMAIN,
        call_id,
        (unsigned int)cseq,
        config->sip_user, local_ip, LOCAL_SIP_PORT);

    if (with_auth && last_auth_challenge.valid) {
        written += build_auth_header(buf + written, len - written,
            config->sip_user, config->sip_password, "REGISTER", uri, &last_auth_challenge);
    }

    written += snprintf(buf + written, len - written,
        "Expires: 120\r\n"
        "User-Agent: ESP32-Doorbell/1.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n");

    return written;
}

// Build SDP body
static int build_sdp(char *buf, size_t len, const char *local_ip) {
    return snprintf(buf, len,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=ESP32 Doorbell\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %d RTP/AVP 0 8 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:101 telephone-event/8000\r\n"
        "a=fmtp:101 0-15\r\n"
        "a=ptime:20\r\n"
        "a=sendrecv\r\n",
        local_ip, local_ip, SIP_RTP_PORT);
}

// Build SIP INVITE message
// Uses static buffers to avoid stack overflow
static int build_invite(char *buf, size_t len, const sip_config_t *config,
                        const char *from_tag, const char *call_id, const char *branch,
                        uint32_t cseq, bool with_auth) {
    // Use static buffers to minimize stack usage
    ip_to_str(get_local_ip(), sip_local_ip, sizeof(sip_local_ip));
    snprintf(sip_target_buf, sizeof(sip_target_buf), "%s@%s", config->sip_target, SIP_DOMAIN);
    snprintf(sip_request_uri, sizeof(sip_request_uri), "sip:%s", sip_target_buf);

    // auth_uri is small and constant, keep on stack
    char auth_uri[32];
    snprintf(auth_uri, sizeof(auth_uri), "sip:%s", SIP_DOMAIN);

    // Use static SDP buffer
    int sdp_len = build_sdp(sip_sdp_buf, sizeof(sip_sdp_buf), sip_local_ip);

    int written = snprintf(buf, len,
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n",
        sip_request_uri,
        sip_local_ip, LOCAL_SIP_PORT, branch,
        config->sip_displayname, config->sip_user, SIP_DOMAIN, from_tag,
        sip_target_buf,
        call_id,
        (unsigned int)cseq,
        config->sip_user, sip_local_ip, LOCAL_SIP_PORT);

    if (with_auth && last_auth_challenge.valid) {
        written += build_auth_header(buf + written, len - written,
            config->sip_user, config->sip_password, "INVITE", auth_uri, &last_auth_challenge);
    }

    written += snprintf(buf + written, len - written,
        "User-Agent: ESP32-Doorbell/1.0\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        sdp_len, sip_sdp_buf);

    return written;
}

// Build SIP CANCEL message
static int build_cancel(char *buf, size_t len, const sip_config_t *config,
                        const char *from_tag, const char *to_tag, const char *call_id,
                        const char *branch, uint32_t cseq) {
    char local_ip[16];
    ip_to_str(get_local_ip(), local_ip, sizeof(local_ip));

    char target[128];
    snprintf(target, sizeof(target), "%s@%s", config->sip_target, SIP_DOMAIN);

    int written = snprintf(buf, len,
        "CANCEL sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s>",
        target,
        local_ip, LOCAL_SIP_PORT, branch,
        config->sip_displayname, config->sip_user, SIP_DOMAIN, from_tag,
        target);

    if (to_tag[0]) {
        written += snprintf(buf + written, len - written, ";tag=%s", to_tag);
    }

    written += snprintf(buf + written, len - written,
        "\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u CANCEL\r\n"
        "User-Agent: ESP32-Doorbell/1.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        call_id,
        (unsigned int)cseq);

    return written;
}

// Build SIP ACK message
static int build_ack(char *buf, size_t len, const sip_config_t *config,
                     const char *from_tag, const char *to_tag, const char *call_id,
                     const char *request_uri, const char *to_target, uint32_t cseq) {
    char local_ip[16];
    ip_to_str(get_local_ip(), local_ip, sizeof(local_ip));

    char branch[64];
    generate_branch(branch, sizeof(branch));

    // Ensure request_uri starts with sip:
    char normalized_uri[256];
    if (strncmp(request_uri, "sip:", 4) == 0) {
        strncpy(normalized_uri, request_uri, sizeof(normalized_uri) - 1);
    } else {
        snprintf(normalized_uri, sizeof(normalized_uri), "sip:%s", request_uri);
    }

    int written = snprintf(buf, len,
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s>",
        normalized_uri,
        local_ip, LOCAL_SIP_PORT, branch,
        config->sip_displayname, config->sip_user, SIP_DOMAIN, from_tag,
        to_target);

    if (to_tag[0]) {
        written += snprintf(buf + written, len - written, ";tag=%s", to_tag);
    }

    written += snprintf(buf + written, len - written,
        "\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u ACK\r\n"
        "User-Agent: ESP32-Doorbell/1.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        call_id,
        (unsigned int)cseq);

    return written;
}

// Build SIP BYE message
static int build_bye(char *buf, size_t len, const sip_config_t *config,
                     const char *from_tag, const char *to_tag, const char *call_id,
                     const char *request_uri, const char *to_target, uint32_t cseq) {
    char local_ip[16];
    ip_to_str(get_local_ip(), local_ip, sizeof(local_ip));

    char branch[64];
    generate_branch(branch, sizeof(branch));

    // Ensure request_uri starts with sip:
    char normalized_uri[256];
    if (strncmp(request_uri, "sip:", 4) == 0) {
        strncpy(normalized_uri, request_uri, sizeof(normalized_uri) - 1);
    } else {
        snprintf(normalized_uri, sizeof(normalized_uri), "sip:%s", request_uri);
    }

    int written = snprintf(buf, len,
        "BYE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s>",
        normalized_uri,
        local_ip, LOCAL_SIP_PORT, branch,
        config->sip_displayname, config->sip_user, SIP_DOMAIN, from_tag,
        to_target);

    if (to_tag[0]) {
        written += snprintf(buf + written, len - written, ";tag=%s", to_tag);
    }

    written += snprintf(buf + written, len - written,
        "\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u BYE\r\n"
        "User-Agent: ESP32-Doorbell/1.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        call_id,
        (unsigned int)cseq);

    return written;
}

// Build OK response
static int build_ok_response(char *buf, size_t len, const char *request) {
    char via[256], from[256], to[256], call_id[64], cseq[64];

    if (!extract_header(request, "Via", via, sizeof(via)) ||
        !extract_header(request, "From", from, sizeof(from)) ||
        !extract_header(request, "To", to, sizeof(to)) ||
        !extract_header(request, "Call-ID", call_id, sizeof(call_id)) ||
        !extract_header(request, "CSeq", cseq, sizeof(cseq))) {
        return 0;
    }

    return snprintf(buf, len,
        "SIP/2.0 200 OK\r\n"
        "Via: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        via, from, to, call_id, cseq);
}

// Send SIP message to specific address
static bool sip_send_to(uint32_t ip, uint16_t port, const char *msg, size_t len) {
    if (sip_socket < 0) return false;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = ip;

    ssize_t sent = sendto(sip_socket, msg, len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));

    if (sent < 0) {
        ESP_LOGW(TAG, "SIP send failed: %d", errno);
        return false;
    }

    return sent == len;
}

// Log SIP message (first line only, or full content if verbose)
static void log_sip_message(const char *prefix, const char *msg, size_t msg_len) {
    // Always log the first line
    const char *end = strstr(msg, "\r\n");
    if (!end) end = msg + strlen(msg);

    size_t line_len = end - msg;
    if (line_len > 100) line_len = 100;  // Truncate very long first lines

    char line_buf[104];
    strncpy(line_buf, msg, line_len);
    line_buf[line_len] = '\0';

    ESP_LOGI(TAG, "%s: %s", prefix, line_buf);

    // If verbose logging is enabled, log the full message
    if (verbose_logging) {
        ESP_LOGI(TAG, "--- %s FULL MESSAGE ---", prefix);
        // Log in chunks to avoid buffer overflow
        const char *ptr = msg;
        size_t remaining = msg_len > 0 ? msg_len : strlen(msg);
        while (remaining > 0) {
            size_t chunk = remaining > 200 ? 200 : remaining;
            // Find a good break point (newline)
            if (chunk < remaining) {
                const char *nl = (const char *)memchr(ptr, '\n', chunk);
                if (nl) chunk = nl - ptr + 1;
            }
            char chunk_buf[204];
            strncpy(chunk_buf, ptr, chunk);
            chunk_buf[chunk] = '\0';
            // Remove trailing \r\n for cleaner output
            char *cr = strchr(chunk_buf, '\r');
            if (cr && cr[1] == '\n' && cr[2] == '\0') *cr = '\0';
            ESP_LOGI(TAG, "%s", chunk_buf);
            ptr += chunk;
            remaining -= chunk;
        }
        ESP_LOGI(TAG, "--- END %s ---", prefix);
    }
}

// Send SIP message to Fritz!Box
static bool sip_send(const char *msg, size_t len) {
    if (!is_sip_network_ready()) return false;

    uint32_t dest_ip;
    if (!resolve_sip_proxy(&dest_ip)) {
        ESP_LOGW(TAG, "Cannot resolve SIP proxy");
        return false;
    }

    // Log destination and message for debugging
    char ip_str[16];
    ip_to_str(dest_ip, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, ">>> SIP TX to %s:%d (%d bytes)", ip_str, SIP_PORT, (int)len);
    log_sip_message("TX", msg, len);

    return sip_send_to(dest_ip, SIP_PORT, msg, len);
}

// Send response back to sender
static bool sip_send_response(const char *msg, size_t len) {
    if (sip_socket < 0) return false;
    if (last_remote_addr.sin_addr.s_addr == 0) return false;

    // Log outgoing response
    char ip_str[16];
    ip_to_str(last_remote_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, ">>> SIP TX to %s:%d (%d bytes)",
             ip_str, ntohs(last_remote_addr.sin_port), (int)len);
    log_sip_message("TX", msg, len);

    ssize_t sent = sendto(sip_socket, msg, len, 0,
                          (struct sockaddr *)&last_remote_addr, sizeof(last_remote_addr));
    return sent == len;
}

// Wait for SIP response with timeout (blocking socket with SO_RCVTIMEO)
static int wait_for_response(char *buf, size_t len, int timeout_ms) {
    // Set receive timeout on socket
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sip_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    socklen_t addr_len = sizeof(last_remote_addr);
    ssize_t received = recvfrom(sip_socket, buf, len - 1, 0,
                                 (struct sockaddr *)&last_remote_addr, &addr_len);

    if (received > 0) {
        buf[received] = '\0';

        // Log received response
        char ip_str[16];
        ip_to_str(last_remote_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "<<< SIP RX from %s:%d (%d bytes)",
                 ip_str, ntohs(last_remote_addr.sin_port), (int)received);
        log_sip_message("RX", buf, received);

        return received;
    }

    return 0;
}

// Reset call session
static void reset_sip_call(void) {
    memset(&sip_call, 0, sizeof(sip_call));
}

// Public API implementations

esp_err_t sip_client_init(void) {
    if (sip_initialized) {
        return ESP_OK;
    }

    // Load verbose logging state from NVS
    load_verbose_logging_state();

    // Create SIP signaling socket
    sip_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sip_socket < 0) {
        ESP_LOGE(TAG, "Failed to create SIP socket");
        return ESP_FAIL;
    }

    // Bind to local port
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(LOCAL_SIP_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sip_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind SIP socket to port %d", LOCAL_SIP_PORT);
        close(sip_socket);
        sip_socket = -1;
        return ESP_FAIL;
    }

    // Keep socket in blocking mode - we use SO_RCVTIMEO for timeouts

    // Create RTP socket
    rtp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rtp_socket >= 0) {
        struct sockaddr_in rtp_addr;
        memset(&rtp_addr, 0, sizeof(rtp_addr));
        rtp_addr.sin_family = AF_INET;
        rtp_addr.sin_port = htons(SIP_RTP_PORT);
        rtp_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(rtp_socket, (struct sockaddr *)&rtp_addr, sizeof(rtp_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind RTP socket");
            close(rtp_socket);
            rtp_socket = -1;
        }
        // Keep RTP socket blocking - use MSG_DONTWAIT for non-blocking ops
    }

    sip_initialized = true;
    ESP_LOGI(TAG, "SIP client initialized on port %d", LOCAL_SIP_PORT);

    return ESP_OK;
}

void sip_client_deinit(void) {
    if (sip_socket >= 0) {
        close(sip_socket);
        sip_socket = -1;
    }
    if (rtp_socket >= 0) {
        close(rtp_socket);
        rtp_socket = -1;
    }
    sip_initialized = false;
}

bool sip_config_load(sip_config_t *config) {
    memset(config, 0, sizeof(*config));

    nvs_handle_t handle;
    if (nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t len;

    len = sizeof(config->sip_user);
    nvs_get_str(handle, NVS_KEY_USER, config->sip_user, &len);

    len = sizeof(config->sip_password);
    nvs_get_str(handle, NVS_KEY_PASSWORD, config->sip_password, &len);

    len = sizeof(config->sip_displayname);
    if (nvs_get_str(handle, NVS_KEY_DISPLAYNAME, config->sip_displayname, &len) != ESP_OK) {
        strcpy(config->sip_displayname, "Doorbell");
    }

    len = sizeof(config->sip_target);
    if (nvs_get_str(handle, NVS_KEY_TARGET, config->sip_target, &len) != ESP_OK) {
        strcpy(config->sip_target, "**11");
    }

    nvs_close(handle);
    return true;
}

esp_err_t sip_config_save(const sip_config_t *config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, NVS_KEY_USER, config->sip_user);
    nvs_set_str(handle, NVS_KEY_PASSWORD, config->sip_password);
    nvs_set_str(handle, NVS_KEY_DISPLAYNAME, config->sip_displayname);
    nvs_set_str(handle, NVS_KEY_TARGET, config->sip_target);

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "SIP config saved");
    return err;
}

bool sip_config_valid(const sip_config_t *config) {
    return config->sip_user[0] != '\0' &&
           config->sip_password[0] != '\0' &&
           config->sip_target[0] != '\0';
}

bool sip_is_enabled(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return true;  // Default to enabled if NVS not available
    }

    uint8_t enabled = 1;
    nvs_get_u8(handle, NVS_KEY_ENABLED, &enabled);
    nvs_close(handle);

    return enabled != 0;
}

esp_err_t sip_set_enabled(bool enabled) {
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "SIP %s", enabled ? "enabled" : "disabled");
    return err;
}

bool sip_verbose_logging(void) {
    return verbose_logging;
}

esp_err_t sip_set_verbose_logging(bool enabled) {
    verbose_logging = enabled;

    // Persist to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, NVS_KEY_VERBOSE, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "SIP verbose logging %s", enabled ? "enabled" : "disabled");
    return err;
}

// Load verbose logging state from NVS (call during init)
static void load_verbose_logging_state(void) {
    nvs_handle_t handle;
    if (nvs_manager_open(NVS_SIP_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        verbose_logging = false;  // Default to disabled
        return;
    }

    uint8_t enabled = 0;
    nvs_get_u8(handle, NVS_KEY_VERBOSE, &enabled);
    nvs_close(handle);

    verbose_logging = (enabled != 0);
    if (verbose_logging) {
        ESP_LOGI(TAG, "Verbose SIP logging enabled from NVS");
    }
}

esp_err_t sip_register(const sip_config_t *config) {
    if (!sip_config_valid(config)) {
        ESP_LOGW(TAG, "SIP config incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_sip_network_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    last_register_attempt_ms = millis();
    last_register_successful = false;

    char tag[32], call_id[64], branch[64];
    generate_tag(tag, sizeof(tag));
    generate_call_id(call_id, sizeof(call_id));
    generate_branch(branch, sizeof(branch));
    uint32_t cseq = 1;

    // Use static buffers to avoid stack overflow
    int msg_len = build_register(sip_msg_buf, sizeof(sip_msg_buf), config, tag, call_id, branch, cseq, false);

    ESP_LOGI(TAG, "Sending REGISTER to fritz.box");
    if (!sip_send(sip_msg_buf, msg_len)) {
        ESP_LOGE(TAG, "Failed to send REGISTER");
        return ESP_FAIL;
    }

    int resp_len = wait_for_response(sip_msg_buf2, sizeof(sip_msg_buf2), SIP_RESPONSE_TIMEOUT_MS);

    if (resp_len > 0) {
        ESP_LOGI(TAG, "REGISTER response: %d", get_status_code(sip_msg_buf2));

        int status = get_status_code(sip_msg_buf2);
        last_register_status = status;

        if (status == 401 || status == 407) {
            ESP_LOGI(TAG, "Authentication required, resending...");

            parse_auth_challenge(sip_msg_buf2, &last_auth_challenge);

            if (last_auth_challenge.valid) {
                cseq++;
                generate_branch(branch, sizeof(branch));
                msg_len = build_register(sip_msg_buf, sizeof(sip_msg_buf), config, tag, call_id, branch, cseq, true);

                ESP_LOGI(TAG, "Sending authenticated REGISTER");
                if (!sip_send(sip_msg_buf, msg_len)) {
                    ESP_LOGE(TAG, "Failed to send authenticated REGISTER");
                    return ESP_FAIL;
                }

                resp_len = wait_for_response(sip_msg_buf2, sizeof(sip_msg_buf2), SIP_RESPONSE_TIMEOUT_MS);
                if (resp_len > 0) {
                    status = get_status_code(sip_msg_buf2);
                    last_register_status = status;

                    if (status >= 200 && status < 300) {
                        ESP_LOGI(TAG, "SIP registration successful");
                        last_register_successful = true;
                        last_register_ok_ms = millis();
                    } else {
                        ESP_LOGW(TAG, "SIP registration failed: %d", status);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Failed to parse auth challenge");
            }
        } else if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "SIP registration successful (no auth)");
            last_register_successful = true;
            last_register_ok_ms = millis();
        } else {
            ESP_LOGW(TAG, "SIP registration failed: %d", status);
        }
    } else {
        ESP_LOGW(TAG, "No response to REGISTER (timeout)");
    }

    last_register_time = millis();
    return last_register_successful ? ESP_OK : ESP_FAIL;
}

void sip_register_if_needed(const sip_config_t *config) {
    uint32_t now = millis();

    // On first call (last_register_time == 0), register immediately
    // Otherwise, wait for REGISTER_INTERVAL_MS between registrations
    if (last_register_time != 0 && (now - last_register_time < REGISTER_INTERVAL_MS)) {
        return;
    }

    if (sip_call.active) return;
    if (!is_sip_network_ready()) return;

    ESP_LOGI(TAG, "Attempting SIP registration...");
    sip_register(config);
}

esp_err_t sip_ring(const sip_config_t *config) {
    if (!sip_config_valid(config)) {
        ESP_LOGW(TAG, "SIP config incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_sip_network_ready()) {
        ESP_LOGW(TAG, "SIP network not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (pending_invite.active) {
        ESP_LOGI(TAG, "SIP ring already active");
        return ESP_ERR_INVALID_STATE;
    }

    if (sip_call.active) {
        ESP_LOGI(TAG, "SIP call already active");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize pending invite
    memset(&pending_invite, 0, sizeof(pending_invite));
    generate_tag(pending_invite.from_tag, sizeof(pending_invite.from_tag));
    generate_call_id(pending_invite.call_id, sizeof(pending_invite.call_id));
    generate_branch(pending_invite.branch, sizeof(pending_invite.branch));
    pending_invite.cseq = 1;
    snprintf(pending_invite.target, sizeof(pending_invite.target), "%s@%s",
             config->sip_target, SIP_DOMAIN);
    memcpy(&pending_invite.config, config, sizeof(sip_config_t));

    // Build and send INVITE (use static buffer to avoid stack overflow)
    int msg_len = build_invite(sip_msg_buf, sizeof(sip_msg_buf), config,
                               pending_invite.from_tag, pending_invite.call_id,
                               pending_invite.branch, pending_invite.cseq, false);

    ESP_LOGI(TAG, "Sending INVITE to %s", config->sip_target);
    if (!sip_send(sip_msg_buf, msg_len)) {
        return ESP_FAIL;
    }

    pending_invite.active = true;
    pending_invite.invite_start_ms = millis();

    return ESP_OK;
}

bool sip_ring_active(void) {
    return pending_invite.active;
}

esp_err_t sip_request_ring(void) {
    if (!sip_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pending_invite.active || sip_call.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ring_requested = true;
    ESP_LOGI(TAG, "SIP ring requested (deferred)");
    return ESP_OK;
}

void sip_check_pending_ring(const sip_config_t *config) {
    if (!ring_requested) return;
    ring_requested = false;

    if (!config || !sip_config_valid(config)) {
        ESP_LOGW(TAG, "Cannot ring: invalid config");
        return;
    }

    esp_err_t err = sip_ring(config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Deferred ring failed: %s", esp_err_to_name(err));
    }
}

void sip_ring_process(void) {
    if (!pending_invite.active) return;

    uint32_t now = millis();

    if (ring_tick_callback) {
        ring_tick_callback();
    }

    if (pending_invite.answered) {
        // Send ACK if not already sent (use static buffer)
        if (!pending_invite.ack_sent) {
            const char *request_uri = pending_invite.remote_target[0] ?
                                      pending_invite.remote_target :
                                      pending_invite.target;

            int msg_len = build_ack(sip_msg_buf, sizeof(sip_msg_buf), &pending_invite.config,
                                    pending_invite.from_tag, pending_invite.to_tag,
                                    pending_invite.call_id, request_uri,
                                    pending_invite.target, pending_invite.cseq);

            ESP_LOGD(TAG, "Sending ACK");
            pending_invite.ack_sent = sip_send(sip_msg_buf, msg_len);
            pending_invite.answered_ms = now;

            if (!pending_invite.ack_sent) {
                pending_invite.active = false;
                reset_sip_call();
                return;
            }

            // Start call session
            sip_call.active = true;
            sip_call.inbound = false;
            sip_call.acked = true;
            strncpy(sip_call.call_id, pending_invite.call_id, sizeof(sip_call.call_id) - 1);
            strncpy(sip_call.local_tag, pending_invite.from_tag, sizeof(sip_call.local_tag) - 1);
            strncpy(sip_call.remote_tag, pending_invite.to_tag, sizeof(sip_call.remote_tag) - 1);
            sip_call.rtp_remote_ip = pending_invite.media.remote_ip;
            sip_call.rtp_remote_port = pending_invite.media.remote_port;
            sip_call.start_ms = now;
            memcpy(&sip_call.config, &pending_invite.config, sizeof(sip_config_t));
        }
        // Send BYE after hold time (use static buffer)
        else if (!pending_invite.bye_sent &&
                 (now - pending_invite.answered_ms > SIP_IN_CALL_HOLD_MS)) {
            const char *request_uri = pending_invite.remote_target[0] ?
                                      pending_invite.remote_target :
                                      pending_invite.target;

            int msg_len = build_bye(sip_msg_buf, sizeof(sip_msg_buf), &pending_invite.config,
                                    pending_invite.from_tag, pending_invite.to_tag,
                                    pending_invite.call_id, request_uri,
                                    pending_invite.target, pending_invite.cseq + 1);

            ESP_LOGD(TAG, "Sending BYE");
            pending_invite.bye_sent = sip_send(sip_msg_buf, msg_len);
            pending_invite.active = false;
            reset_sip_call();
        }
        return;
    }

    // Check for ring timeout - send CANCEL (use static buffer)
    if (now - pending_invite.invite_start_ms >= SIP_RING_DURATION_MS) {
        if (pending_invite.can_cancel && !pending_invite.cancel_sent) {
            int msg_len = build_cancel(sip_msg_buf, sizeof(sip_msg_buf), &pending_invite.config,
                                       pending_invite.from_tag, pending_invite.to_tag,
                                       pending_invite.call_id, pending_invite.branch,
                                       pending_invite.cseq);

            ESP_LOGI(TAG, "Ring timeout, sending CANCEL");
            pending_invite.cancel_sent = sip_send(sip_msg_buf, msg_len);
            pending_invite.cancel_start_ms = now;

            if (!pending_invite.cancel_sent) {
                pending_invite.active = false;
                reset_sip_call();
            }
        } else if (!pending_invite.can_cancel && !pending_invite.cancel_sent) {
            ESP_LOGI(TAG, "Skipping CANCEL (no provisional response)");
            pending_invite.active = false;
            reset_sip_call();
        }
    }

    // Wait for 487 after CANCEL
    if (pending_invite.cancel_sent && pending_invite.cancel_start_ms > 0 &&
        (now - pending_invite.cancel_start_ms > SIP_CANCEL_WAIT_MS)) {
        pending_invite.active = false;
        reset_sip_call();
    }
}

void sip_handle_incoming(void) {
    if (!is_sip_network_ready()) return;

    char buf[SIP_MSG_BUF_SIZE];
    socklen_t addr_len = sizeof(last_remote_addr);

    // Use MSG_DONTWAIT for non-blocking receive (socket is blocking by default)
    ssize_t len = recvfrom(sip_socket, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&last_remote_addr, &addr_len);
    if (len <= 0) return;

    buf[len] = '\0';

    // Log received message
    char ip_str[16];
    ip_to_str(last_remote_addr.sin_addr.s_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "<<< SIP RX from %s:%d (%d bytes)",
             ip_str, ntohs(last_remote_addr.sin_port), (int)len);
    log_sip_message("RX", buf, len);

    // Check if it's a response or request
    if (strncmp(buf, "SIP/2.0", 7) != 0) {
        // It's a request
        char *space = strchr(buf, ' ');
        if (!space) return;

        char method[16];
        size_t method_len = space - buf;
        if (method_len >= sizeof(method)) method_len = sizeof(method) - 1;
        strncpy(method, buf, method_len);
        method[method_len] = '\0';

        if (strcasecmp(method, "OPTIONS") == 0) {
            // Use static buffer to avoid stack overflow
            int resp_len = build_ok_response(sip_msg_buf, sizeof(sip_msg_buf), buf);
            if (resp_len > 0) {
                ESP_LOGD(TAG, "Responding to OPTIONS");
                sip_send_response(sip_msg_buf, resp_len);
            }
            return;
        }

        if (strcasecmp(method, "BYE") == 0 || strcasecmp(method, "CANCEL") == 0) {
            // Use static buffer to avoid stack overflow
            int resp_len = build_ok_response(sip_msg_buf, sizeof(sip_msg_buf), buf);
            if (resp_len > 0) {
                sip_send_response(sip_msg_buf, resp_len);
            }
            pending_invite.active = false;
            reset_sip_call();
            return;
        }

        // Handle incoming INVITE (for future use)
        if (strcasecmp(method, "INVITE") == 0) {
            // For now, respond with Busy Here if we're not ready
            ESP_LOGI(TAG, "Incoming INVITE (not supported yet)");
            // Could implement full inbound call handling here
            return;
        }

        return;
    }

    // It's a response - check if it matches our pending invite
    if (!pending_invite.active) return;

    char resp_call_id[64];
    if (!extract_header(buf, "Call-ID", resp_call_id, sizeof(resp_call_id))) return;
    if (strcasecmp(resp_call_id, pending_invite.call_id) != 0) return;

    uint32_t resp_cseq;
    char resp_method[16];
    if (!parse_cseq(buf, &resp_cseq, resp_method, sizeof(resp_method))) return;
    if (strcasecmp(resp_method, "INVITE") != 0) return;

    int status = get_status_code(buf);
    bool is_current = (resp_cseq == pending_invite.cseq);

    // Extract To tag and Contact for later use
    char to_tag[32];
    if (extract_to_tag(buf, to_tag, sizeof(to_tag))) {
        if (to_tag[0] && is_current) {
            strncpy(pending_invite.to_tag, to_tag, sizeof(pending_invite.to_tag) - 1);
        }
    }

    char contact_uri[256];
    if (extract_contact_uri(buf, contact_uri, sizeof(contact_uri))) {
        if (contact_uri[0] && is_current) {
            strncpy(pending_invite.remote_target, contact_uri, sizeof(pending_invite.remote_target) - 1);
        }
    }

    // Parse SDP if present
    const char *sdp = extract_sdp_body(buf);
    if (sdp && is_current) {
        parse_sdp_media(sdp, last_remote_addr.sin_addr.s_addr, &pending_invite.media);
        pending_invite.media_ready = (pending_invite.media.remote_port > 0);
    }

    if (status == 401 || status == 407) {
        // Need to send ACK for non-2xx responses
        // Use static buffers to avoid stack overflow
        char branch[64];
        extract_via_branch(buf, branch, sizeof(branch));

        int ack_len = build_ack(sip_msg_buf2, sizeof(sip_msg_buf2), &pending_invite.config,
                                pending_invite.from_tag, to_tag, pending_invite.call_id,
                                pending_invite.target, pending_invite.target, resp_cseq);
        if (ack_len > 0) {
            sip_send(sip_msg_buf2, ack_len);
        }

        if (!is_current || pending_invite.auth_sent) return;

        ESP_LOGI(TAG, "INVITE needs authentication");
        parse_auth_challenge(buf, &last_auth_challenge);

        if (last_auth_challenge.valid) {
            pending_invite.cseq++;
            generate_branch(pending_invite.branch, sizeof(pending_invite.branch));

            int msg_len = build_invite(sip_msg_buf, sizeof(sip_msg_buf), &pending_invite.config,
                                       pending_invite.from_tag, pending_invite.call_id,
                                       pending_invite.branch, pending_invite.cseq, true);

            pending_invite.auth_sent = true;
            ESP_LOGI(TAG, "Sending authenticated INVITE");
            if (!sip_send(sip_msg_buf, msg_len)) {
                ESP_LOGE(TAG, "Failed to send authenticated INVITE");
                pending_invite.active = false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse INVITE auth challenge");
        }
        return;
    }

    if (status >= 100 && status < 200) {
        ESP_LOGI(TAG, "Received %d provisional response", status);
        // Provisional response - we can now send CANCEL if needed
        if (is_current) {
            pending_invite.can_cancel = true;
        }
        return;
    }

    if (status >= 200 && status < 300) {
        // Success - call was answered
        if (is_current) {
            ESP_LOGI(TAG, "Call answered");
            pending_invite.can_cancel = false;
            pending_invite.answered = true;
        }
        return;
    }

    if (status >= 300) {
        // Final error response - need to ACK (use static buffer)
        char branch[64];
        extract_via_branch(buf, branch, sizeof(branch));

        int ack_len = build_ack(sip_msg_buf, sizeof(sip_msg_buf), &pending_invite.config,
                                pending_invite.from_tag, to_tag, pending_invite.call_id,
                                pending_invite.target, pending_invite.target, resp_cseq);
        if (ack_len > 0) {
            sip_send(sip_msg_buf, ack_len);
        }

        if (is_current) {
            ESP_LOGW(TAG, "INVITE failed with status %d", status);
            pending_invite.active = false;
        }
    }
}

void sip_media_process(void) {
    // RTP media handling - placeholder for Phase 5
    // Full implementation requires ESP-ADF for audio encoding/decoding
    if (!sip_call.active || !sip_call.acked) return;

    // For now, just check for timeout and send BYE
    // Full RTP handling will be added in Phase 5
}

void sip_get_status(sip_status_t *status) {
    status->registered = last_register_successful;
    status->last_register_ms = last_register_attempt_ms;
    status->last_ok_ms = last_register_ok_ms;
    status->last_status_code = last_register_status;
}

bool sip_is_registered(void) {
    if (!last_register_successful) return false;
    if (last_register_attempt_ms == 0) return false;

    uint32_t now = millis();
    return (now - last_register_ok_ms) <= (REGISTER_INTERVAL_MS * 2);
}

void sip_set_ring_tick_callback(sip_ring_tick_cb_t callback) {
    ring_tick_callback = callback;
}

void sip_set_dtmf_callback(sip_dtmf_cb_t callback) {
    dtmf_callback = callback;
}

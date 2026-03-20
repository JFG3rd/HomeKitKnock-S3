/**
 * RTSP Server Implementation
 *
 * Custom RTSP 1.0 server streaming MJPEG over RTP.
 * Ported from Arduino version (src_arduino/rtsp_server.cpp).
 *
 * Implements:
 * - RFC 2326: Real Time Streaming Protocol (RTSP)
 * - RFC 2435: RTP Payload Format for JPEG-compressed Video
 *
 * Supports TCP interleaved and UDP unicast transport.
 * Video: MJPEG (PT=26, RFC 2435)
 * Audio: AAC-LC (PT=96, RFC 3640 AAC-hbr) — Phase 5
 */

#include "rtsp_server.h"
#include "camera.h"
#include "status_led.h"
#include "audio_capture.h"
#include "aac_encoder_pipe.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include <fcntl.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_heap_caps.h"

static const char *TAG = "rtsp";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define RTSP_PORT           8554
#define MAX_SESSIONS        2
#define SERVER_TASK_STACK   8192
#define SERVER_CORE         1
#define MAX_RTP_PAYLOAD     1192    // 1200 - 8 (JPEG header)
#define JPEG_HEADER_SIZE    8
#define SESSION_TIMEOUT_MS  60000
#define FRAME_INTERVAL_MS   67      // ~15 fps
#define REQ_BUF_SIZE        2048
#define HANDSHAKE_TIMEOUT_S 10
#define TCP_SEND_TIMEOUT_MS 5000
// Disable single-packet TCP fast path while debugging ffmpeg/Scrypted MJPEG probe issues.
// Force all frames through the standard fragment path that includes full RFC2435 features.
#define TCP_SINGLE_RTP_MAX   32768  // Send each JPEG in one RTP packet over TCP (M=1 always; helps client frame detection)
// Interop mode: include RFC2435 quantization tables in first fragment (Q=255).
// Disabled: Arduino version (which worked) used Q=80 with predefined tables.
// Re-enable once basic video is confirmed working.
#define RTP_JPEG_EXPLICIT_QTABLES 0
// Toggle SDP audio advertisement (AAC track2).
// Enabled: AAC audio track (RFC 3640 AAC-hbr, PT=96).
#define RTSP_ADVERTISE_AUDIO 1
// Debug isolation toggle: advertise/setup audio track but suppress RTP audio payload sends.
// Useful to verify whether interleaved AAC packets are corrupting MJPEG probe/decoding.
#define RTSP_SEND_AUDIO_RTP 1
// Diagnostic interop toggle: send full JPEG payload (SOI..EOI) instead of scan-only data.
// Non-standard for RFC2435, but useful to validate decoder expectations in ffmpeg/scrypted.
#define RTP_JPEG_PAYLOAD_FULL_FRAME 0
// Compatibility toggle: keep JPEG EOI (0xFFD9) in RTP payload scan data.
// Some client/prober combinations appear more tolerant when EOI is present.
#define RTP_JPEG_INCLUDE_EOI 0
// Compatibility toggle: force RTP/JPEG type to 1 (ffmpeg 4:2:0) regardless of sensor subsampling.
// NOTE: Disabled — OV2640 outputs 4:2:2. ffmpeg type 0 = 4:2:2 (pre-RFC-2435 convention).
#define RTP_JPEG_FORCE_TYPE_420 0
// Diagnostic fallback: if scan parsing fails, send full JPEG payload anyway.
// Helps confirm whether failure is parser-only vs transport/interop.
#define RTP_JPEG_FORCE_FULL_ON_PARSE_FAIL 1

// UDP backoff
#define UDP_BACKOFF_BASE_MS  50
#define UDP_BACKOFF_MAX_MS   500

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
typedef struct {
    int ctrl_sock;                  // RTSP control TCP socket
    int udp_rtp_sock;               // Video RTP UDP socket (-1 if not used)
    uint16_t client_rtp_port;       // Client video RTP port (UDP)
    uint16_t client_rtcp_port;      // Client video RTCP port (UDP)
    uint16_t server_rtp_port;       // Local video RTP port (UDP)
    uint16_t server_rtcp_port;      // Local video RTCP port (UDP)
    struct sockaddr_in client_addr; // Client address (for UDP sendto)

    uint32_t session_id;
    uint16_t seq_num;               // Video RTP sequence number
    uint32_t timestamp;             // Video RTP timestamp (90kHz)
    uint32_t ssrc;                  // Video SSRC

    bool is_playing;
    bool use_tcp;                   // TCP interleaved mode
    uint8_t tcp_rtp_channel;        // Interleaved RTP channel
    uint8_t tcp_rtcp_channel;       // Interleaved RTCP channel

    uint32_t last_frame_ms;
    uint32_t last_activity_ms;

    // Audio track (PT=96, AAC-hbr)
    bool audio_setup;
    bool audio_use_tcp;
    uint8_t audio_tcp_rtp_channel;
    uint8_t audio_tcp_rtcp_channel;
    int udp_audio_rtp_sock;
    uint16_t audio_client_rtp_port;
    uint16_t audio_client_rtcp_port;
    uint16_t audio_server_rtp_port;
    uint16_t audio_server_rtcp_port;
    uint16_t audio_seq_num;
    uint32_t audio_timestamp;
    uint32_t audio_ssrc;
    uint32_t last_audio_ms;
    bool video_diag_logged;
    bool video_send_diag_logged;
    bool audio_tx_disabled_logged;

    // UDP backoff
    uint32_t udp_backoff_until_ms;
    uint8_t udp_fail_streak;

    // Audio timestamp sync
    uint32_t play_start_ms;       // Set at PLAY time for audio timestamp sync
    bool audio_ts_initialized;    // True after first audio RTP timestamp is anchored
} rtsp_session_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static volatile bool server_running = false;
static rtsp_session_t *sessions[MAX_SESSIONS] = {NULL};
static uint32_t g_udp_fail_total = 0;
static TaskHandle_t server_task_handle = NULL;
static bool allow_udp = false;
static bool s_low_latency_mode = false;
static uint16_t last_frame_width = 0;
static uint16_t last_frame_height = 0;

// RTSP Basic Auth credentials (empty = no auth required)
static char rtsp_auth_user[32] = {0};
static char rtsp_auth_pass[64] = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Simple base64 decoder. Returns number of decoded bytes, 0 on error.
static size_t b64_decode(const char *in, size_t in_len, char *out, size_t out_cap) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    size_t out_len = 0;
    int v = 0, bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        int c = T[(uint8_t)in[i]];
        if (c < 0) continue;
        v = (v << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= out_cap) return 0;
            out[out_len++] = (char)((v >> bits) & 0xFF);
        }
    }
    return out_len;
}

// Returns true if the request carries valid Basic credentials (or no auth is configured).
static bool rtsp_check_auth(const char *req) {
    if (rtsp_auth_user[0] == '\0') return true;  // No auth configured
    const char *hdr = strstr(req, "Authorization: Basic ");
    if (!hdr) return false;
    hdr += 21;
    size_t token_len = strcspn(hdr, "\r\n");
    char decoded[96];
    size_t dec_len = b64_decode(hdr, token_len, decoded, sizeof(decoded) - 1);
    if (dec_len == 0) return false;
    decoded[dec_len] = '\0';
    char expected[96];
    snprintf(expected, sizeof(expected), "%s:%s", rtsp_auth_user, rtsp_auth_pass);
    return (strcmp(decoded, expected) == 0);
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * Send all bytes, handling partial writes (same as mjpeg_server)
 */
static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(sock, p, remaining, 0);
        if (sent < 0) return -1;
        p += sent;
        remaining -= sent;
    }
    return (int)len;
}

/**
 * Reliable send of one complete interleaved RTP packet.
 * Returns:
 *   1 = full packet sent
 *  -1 = socket error/timeout (close session)
 *
 * NOTE: Uses blocking send with socket SO_SNDTIMEO configured during setup.
 * This avoids silent RTP frame drops that can break client probing.
 */
static int send_packet_nonblock(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(sock, p, remaining, 0);
        if (sent > 0) {
            p += sent;
            remaining -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 1;
}

/**
 * Receive until \r\n\r\n or timeout.
 * Returns number of bytes read, or -1 on error/timeout.
 */
static int recv_request(int sock, char *buf, size_t buf_size, int timeout_sec) {
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int total = 0;
    while (total < (int)(buf_size - 1)) {
        int n = recv(sock, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) {
            if (total > 0) break;
            return -1;
        }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

/**
 * Non-blocking read of available data on socket.
 * Returns bytes read, 0 if nothing, -1 on error.
 */
static int recv_nonblock(int sock, char *buf, size_t buf_size) {
    int n = recv(sock, buf, buf_size - 1, MSG_DONTWAIT);
    if (n > 0) {
        buf[n] = '\0';
        return n;
    }
    if (n == 0) return -1; // Connection closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

// ---------------------------------------------------------------------------
// RTSP request parsing
// ---------------------------------------------------------------------------

static void parse_method(const char *req, char *method, size_t method_size) {
    const char *sp = strchr(req, ' ');
    if (sp && (size_t)(sp - req) < method_size) {
        memcpy(method, req, sp - req);
        method[sp - req] = '\0';
    } else {
        method[0] = '\0';
    }
}

static int parse_cseq(const char *req) {
    const char *p = strstr(req, "CSeq:");
    if (!p) p = strstr(req, "cseq:");
    if (!p) return 1;
    return atoi(p + 5);
}

static bool parse_transport(const char *req, char *transport, size_t size) {
    const char *p = strstr(req, "Transport:");
    if (!p) return false;
    p += 10;
    while (*p == ' ') p++;
    const char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);
    size_t len = end - p;
    if (len >= size) len = size - 1;
    memcpy(transport, p, len);
    transport[len] = '\0';
    return true;
}

static bool parse_session_id(const char *req, uint32_t *session_id) {
    const char *p = strstr(req, "Session:");
    if (!p) return false;
    p += 8;
    while (*p == ' ') p++;
    *session_id = strtoul(p, NULL, 16);
    return *session_id != 0;
}

static bool parse_interleaved(const char *transport, uint8_t *rtp_ch, uint8_t *rtcp_ch) {
    const char *p = strstr(transport, "interleaved=");
    if (!p) return false;
    p += 12;
    *rtp_ch = (uint8_t)atoi(p);
    const char *dash = strchr(p, '-');
    if (dash) {
        *rtcp_ch = (uint8_t)atoi(dash + 1);
    } else {
        *rtcp_ch = *rtp_ch + 1;
    }
    return true;
}

static bool parse_client_ports(const char *transport, uint16_t *rtp_port, uint16_t *rtcp_port) {
    const char *p = strstr(transport, "client_port=");
    if (!p) return false;
    p += 12;
    *rtp_port = (uint16_t)atoi(p);
    const char *dash = strchr(p, '-');
    if (dash) {
        *rtcp_port = (uint16_t)atoi(dash + 1);
    } else {
        *rtcp_port = *rtp_port + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

static int find_free_slot(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i]) return i;
    }
    return -1;
}

static int find_slot_by_id(uint32_t session_id) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i] && sessions[i]->session_id == session_id) return i;
    }
    return -1;
}

static int find_slot_by_sock(int sock) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i] && sessions[i]->ctrl_sock == sock) return i;
    }
    return -1;
}

static int find_slot_by_ptr(const rtsp_session_t *s) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i] == s) return i;
    }
    return -1;
}

static void cleanup_session(int slot, const char *reason) {
    if (slot < 0 || slot >= MAX_SESSIONS || !sessions[slot]) return;
    rtsp_session_t *s = sessions[slot];

    ESP_LOGI(TAG,
             "Session close reason=%s sid=%08lx slot=%d playing=%d audio_setup=%d seq=%u aseq=%u ctrl=%d",
             reason ? reason : "unspecified",
             (unsigned long)s->session_id,
             slot,
             s->is_playing,
             s->audio_setup,
             (unsigned int)s->seq_num,
             (unsigned int)s->audio_seq_num,
             s->ctrl_sock);

    if (s->udp_rtp_sock >= 0) {
        close(s->udp_rtp_sock);
    }
    if (s->udp_audio_rtp_sock >= 0) {
        close(s->udp_audio_rtp_sock);
    }
    if (s->ctrl_sock >= 0) {
        close(s->ctrl_sock);
    }
    free(s);
    sessions[slot] = NULL;
    ESP_LOGI(TAG, "Session cleaned up (slot %d), free heap: %lu",
             slot, (unsigned long)esp_get_free_heap_size());
}

static void reap_dead_sessions(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        rtsp_session_t *s = sessions[i];
        if (!s) continue;

        if (s->ctrl_sock < 0) {
            ESP_LOGI(TAG, "Reaping dead session %08lx from slot %d",
                     (unsigned long)s->session_id, i);
            cleanup_session(i, "reap_dead_ctrl_sock");
        }
    }
}

// ---------------------------------------------------------------------------
// JPEG scan data parser (RFC 2435)
// ---------------------------------------------------------------------------

#if RTP_JPEG_EXPLICIT_QTABLES
typedef struct {
    uint8_t data[128];
    uint16_t len;
    bool has_luma;
    bool has_chroma;
} jpeg_qtables_t;
#endif

/**
 * Parse JPEG to find entropy-coded scan data offset.
 * Sets type per RFC 2435 and returns offset to scan data.
 * RFC 2435 type mapping: type 0 = 4:2:2, type 1 = 4:2:0.
 * Returns 0 if JPEG is invalid or SOS not found.
 */
static size_t find_jpeg_scan_data(const uint8_t *jpeg, size_t len,
                                   uint8_t *type, uint8_t *q,
                                   uint16_t *restart_interval) {
    // ffmpeg convention (pre-RFC-2435): type 0 = 4:2:2, type 1 = 4:2:0.
    // RFC 2435 §3.1 defines the opposite but ffmpeg does NOT follow it.
    // OV2640 outputs 4:2:2 natively, so default to type 0.
    *type = 0;  // Default: YUV 4:2:2 (ffmpeg: type 0 = 4:2:2)
    *q = 80;    // Legacy default static Q (no explicit quant tables in RTP/JPEG)
    if (restart_interval) *restart_interval = 0;

    if (len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return 0;
    }

    size_t i = 2;
    while (i < len - 1) {
        // Recover by scanning forward to next marker introducer.
        if (jpeg[i] != 0xFF) {
            while (i < len - 1 && jpeg[i] != 0xFF) {
                i++;
            }
            if (i >= len - 1) {
                return 0;
            }
        }

        // Skip repeated 0xFF fill bytes.
        while (i < len - 1 && jpeg[i + 1] == 0xFF) {
            i++;
        }

        // Ignore stuffed 0xFF00 if encountered before SOS.
        if (i < len - 1 && jpeg[i + 1] == 0x00) {
            i += 2;
            continue;
        }

        uint8_t marker = jpeg[i + 1];
        i += 2;

        // SOF0: determine chroma subsampling from Y component sampling factors.
        if (marker == 0xC0) {
            if (i + 2 > len) return 0;
            uint16_t seg_len = (uint16_t)((jpeg[i] << 8) | jpeg[i + 1]);
            if (seg_len < 8 || i + seg_len > len) return 0;

            uint8_t components = jpeg[i + 7];
            size_t comp_off = i + 8;
            for (uint8_t comp = 0; comp < components; comp++) {
                if (comp_off + 2 >= i + seg_len) return 0;
                uint8_t comp_id = jpeg[comp_off];
                uint8_t sampling = jpeg[comp_off + 1];
                if (comp_id == 1) {
                    if (sampling == 0x21) {
                        *type = 0; // ffmpeg type 0: YUV 4:2:2 (H=2, V=1)
                    } else if (sampling == 0x22) {
                        *type = 1; // ffmpeg type 1: YUV 4:2:0 (H=2, V=2)
                    }
                    break;
                }
                comp_off += 3;
            }
        }

        // DRI: restart interval in MCUs (RFC 2435 type bit 0x40)
        if (marker == 0xDD && restart_interval) {
            if (i + 4 > len) return 0;
            uint16_t seg_len = (uint16_t)((jpeg[i] << 8) | jpeg[i + 1]);
            if (seg_len != 4 || i + seg_len > len) return 0;
            *restart_interval = (uint16_t)((jpeg[i + 2] << 8) | jpeg[i + 3]);
        }

        // SOS: scan data follows after marker length
        if (marker == 0xDA) {
            if (i + 2 > len) return 0;
            uint16_t sos_len = (jpeg[i] << 8) | jpeg[i + 1];
            return i + sos_len;
        }

        // Skip marker data (except standalone markers)
        if (marker != 0xD8 && marker != 0xD9 && (marker < 0xD0 || marker > 0xD7)) {
            if (i + 2 > len) return 0;
            uint16_t marker_len = (jpeg[i] << 8) | jpeg[i + 1];
            i += marker_len;
        }
    }

    return 0;
}

/**
 * Extract JPEG quantization tables (DQT) for RFC 2435 first-fragment header.
 * Returns true when both luma/chroma 8-bit tables are present.
 */
#if RTP_JPEG_EXPLICIT_QTABLES
static bool extract_jpeg_qtables(const uint8_t *jpeg, size_t len, jpeg_qtables_t *qt) {
    if (!jpeg || !qt || len < 4) return false;
    memset(qt, 0, sizeof(*qt));

    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }

    size_t i = 2;
    while (i + 3 < len) {
        if (jpeg[i] != 0xFF) return false;
        uint8_t marker = jpeg[i + 1];
        i += 2;

        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        if (i + 2 > len) return false;
        uint16_t marker_len = (uint16_t)((jpeg[i] << 8) | jpeg[i + 1]);
        if (marker_len < 2 || i + marker_len > len) return false;

        if (marker == 0xDB) {
            size_t p = i + 2;
            size_t end = i + marker_len;
            while (p < end) {
                uint8_t pq_tq = jpeg[p++];
                uint8_t pq = (pq_tq >> 4) & 0x0F;
                uint8_t tq = pq_tq & 0x0F;
                if (pq != 0) {
                    return false; // only 8-bit tables are supported here
                }
                if (p + 64 > end) return false;

                if (tq == 0) {
                    memcpy(qt->data, jpeg + p, 64);
                    qt->has_luma = true;
                } else if (tq == 1) {
                    memcpy(qt->data + 64, jpeg + p, 64);
                    qt->has_chroma = true;
                }
                p += 64;
            }
        }

        if (marker == 0xDA) {
            break; // no more relevant headers after SOS
        }

        i += marker_len;
    }

    if (qt->has_luma && qt->has_chroma) {
        qt->len = 128;
        return true;
    }
    return false;
}
#endif

// ---------------------------------------------------------------------------
// RTSP response helpers
// ---------------------------------------------------------------------------

static void send_rtsp_response(int sock, int cseq, const char *status,
                                const char *extra_headers) {
    char buf[512];
    int len;
    if (extra_headers && extra_headers[0]) {
        len = snprintf(buf, sizeof(buf),
                       "RTSP/1.0 %s\r\nCSeq: %d\r\n%s\r\n",
                       status, cseq, extra_headers);
    } else {
        len = snprintf(buf, sizeof(buf),
                       "RTSP/1.0 %s\r\nCSeq: %d\r\n\r\n",
                       status, cseq);
    }
    send_all(sock, buf, len);
}

// ---------------------------------------------------------------------------
// RTSP method handlers
// ---------------------------------------------------------------------------

static void handle_options(int sock, int cseq) {
    send_rtsp_response(sock, cseq, "200 OK",
                       "Public: DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");
}

static void handle_describe(int sock, int cseq, const char *local_ip) {
    char sdp[768];
    int sdp_len;
    uint16_t sdp_width = last_frame_width;
    uint16_t sdp_height = last_frame_height;

    // Fully defensive probe: tolerate OV2640 warm-up and bad frames.
    if (sdp_width < 16 || sdp_height < 16) {
        ESP_LOGW(TAG, "DESCRIBE: probing camera for dimensions (last=%ux%u)",
                 sdp_width, sdp_height);

        camera_fb_t *probe_fb = NULL;
        const int max_tries = 5;
        for (int i = 0; i < max_tries; i++) {
            probe_fb = camera_capture();
            if (!probe_fb) {
                ESP_LOGW(TAG, "DESCRIBE: camera_capture() returned NULL (try %d/%d)",
                         i + 1, max_tries);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (probe_fb->width >= 16 && probe_fb->height >= 16) {
                sdp_width = probe_fb->width;
                sdp_height = probe_fb->height;
                last_frame_width = sdp_width;
                last_frame_height = sdp_height;
                ESP_LOGI(TAG, "DESCRIBE: probed dimensions %ux%u",
                         sdp_width, sdp_height);
                camera_return_fb(probe_fb);
                probe_fb = NULL;
                break;
            }

            ESP_LOGW(TAG, "DESCRIBE: invalid probe frame %ux%u (try %d/%d)",
                     probe_fb->width, probe_fb->height, i + 1, max_tries);
            camera_return_fb(probe_fb);
            probe_fb = NULL;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (sdp_width < 16 || sdp_height < 16) {
            // Final fallback: hard-coded safe default.
            sdp_width = 640;
            sdp_height = 480;
            last_frame_width = sdp_width;
            last_frame_height = sdp_height;
            ESP_LOGW(TAG, "DESCRIBE: using fallback dimensions %ux%u",
                     sdp_width, sdp_height);
        }
    }

    sdp_len = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=ESP32-S3 Camera\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "a=control:rtsp://%s:%d/mjpeg/1\r\n"
        "m=video 0 RTP/AVP 26\r\n"
        "a=rtpmap:26 JPEG/90000\r\n"
        "a=framesize:26 %u-%u\r\n"
        "a=framerate:15\r\n"
        "a=control:rtsp://%s:%d/mjpeg/1/track1\r\n",
        local_ip, local_ip, local_ip, RTSP_PORT,
        sdp_width, sdp_height,
        local_ip, RTSP_PORT);

#if RTSP_ADVERTISE_AUDIO
    if (audio_capture_is_enabled()) {
        char rtpmap[64], fmtp[256];
        aac_encoder_pipe_get_sdp_rtpmap(rtpmap, sizeof(rtpmap));
        aac_encoder_pipe_get_sdp_fmtp(fmtp, sizeof(fmtp));
        sdp_len += snprintf(sdp + sdp_len, sizeof(sdp) - sdp_len,
            "m=audio 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 %s\r\n"
            "a=fmtp:96 %s\r\n"
            "a=control:rtsp://%s:%d/mjpeg/1/track2\r\n",
            rtpmap, fmtp, local_ip, RTSP_PORT);
    }
#endif

    char headers[256];
    snprintf(headers, sizeof(headers),
             "Content-Base: rtsp://%s:%d/mjpeg/1/\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %d\r\n",
             local_ip, RTSP_PORT, sdp_len);

    char response[1280];
    int rlen = snprintf(response, sizeof(response),
                        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s\r\n%s",
                        cseq, headers, sdp);
    send_all(sock, response, rlen);
}

static bool handle_setup(int sock, int cseq, const char *req,
                          const char *transport_hdr,
                          struct sockaddr_in *client_addr) {
    bool is_audio = (strstr(req, "track2") != NULL);
    bool use_tcp = (strstr(transport_hdr, "RTP/AVP/TCP") != NULL);
    uint8_t rtp_ch = 0, rtcp_ch = 1;
    uint16_t client_rtp_port = 0, client_rtcp_port = 0;

    // Reject audio track if mic not enabled
    if (is_audio && (!RTSP_ADVERTISE_AUDIO || !audio_capture_is_enabled())) {
        send_rtsp_response(sock, cseq, "404 Not Found", NULL);
        return false;
    }

    if (use_tcp) {
        if (!parse_interleaved(transport_hdr, &rtp_ch, &rtcp_ch)) {
            rtp_ch = is_audio ? 2 : 0;
            rtcp_ch = rtp_ch + 1;
        }
    } else {
        if (!allow_udp) {
            ESP_LOGW(TAG, "UDP requested but disabled");
            send_rtsp_response(sock, cseq, "461 Unsupported Transport", NULL);
            return false;
        }
        if (!parse_client_ports(transport_hdr, &client_rtp_port, &client_rtcp_port)) {
            send_rtsp_response(sock, cseq, "461 Unsupported Transport", NULL);
            return false;
        }
    }

    // Check for existing session on this socket (second SETUP for audio track)
    rtsp_session_t *session = NULL;
    int slot = -1;
    uint32_t requested_id = 0;

    if (parse_session_id(req, &requested_id)) {
        slot = find_slot_by_id(requested_id);
        if (slot < 0) {
            send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
            return false;
        }
        session = sessions[slot];
    } else {
        slot = find_free_slot();
        if (slot < 0) {
            reap_dead_sessions();
            slot = find_free_slot();
        }
        if (slot < 0) {
            send_rtsp_response(sock, cseq, "453 Not Enough Bandwidth", NULL);
            return false;
        }

        session = calloc(1, sizeof(rtsp_session_t));
        if (!session) {
            send_rtsp_response(sock, cseq, "500 Internal Server Error", NULL);
            return false;
        }

        session->ctrl_sock = sock;
        session->udp_rtp_sock = -1;
        session->udp_audio_rtp_sock = -1;
        session->session_id = (now_ms() & 0xFFFFFF) | (esp_random() & 0xFF000000);
        session->ssrc = esp_random();
        session->client_addr = *client_addr;
        session->last_activity_ms = now_ms();

        sessions[slot] = session;
    }

    // Store transport per track
    if (is_audio) {
        session->audio_use_tcp = use_tcp;
        session->audio_ssrc = esp_random();
        if (use_tcp) {
            session->audio_tcp_rtp_channel = rtp_ch;
            session->audio_tcp_rtcp_channel = rtcp_ch;
        } else {
            session->audio_client_rtp_port = client_rtp_port;
            session->audio_client_rtcp_port = client_rtcp_port;

            if (session->udp_audio_rtp_sock < 0) {
                session->udp_audio_rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (session->udp_audio_rtp_sock < 0) {
                    ESP_LOGE(TAG, "Failed to create audio UDP socket: errno=%d", errno);
                    send_rtsp_response(sock, cseq, "500 Internal Server Error", NULL);
                    return false;
                }

                struct sockaddr_in local = {
                    .sin_family = AF_INET,
                    .sin_addr.s_addr = htonl(INADDR_ANY),
                    .sin_port = htons(0),
                };
                if (bind(session->udp_audio_rtp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                    ESP_LOGE(TAG, "Audio UDP bind failed: errno=%d", errno);
                    send_rtsp_response(sock, cseq, "500 Internal Server Error", NULL);
                    return false;
                }

                socklen_t local_len = sizeof(local);
                if (getsockname(session->udp_audio_rtp_sock, (struct sockaddr *)&local, &local_len) == 0) {
                    session->audio_server_rtp_port = ntohs(local.sin_port);
                    session->audio_server_rtcp_port = session->audio_server_rtp_port + 1;
                }
            }

            char client_ip[16] = {0};
            inet_ntop(AF_INET, &session->client_addr.sin_addr, client_ip, sizeof(client_ip));
            ESP_LOGI(TAG, "UDP audio destination parsed: %s:%u rtcp=%u (server=%u-%u)",
                     client_ip,
                     (unsigned int)session->audio_client_rtp_port,
                     (unsigned int)session->audio_client_rtcp_port,
                     (unsigned int)session->audio_server_rtp_port,
                     (unsigned int)session->audio_server_rtcp_port);
        }
        session->audio_setup = true;
        ESP_LOGI(TAG, "SETUP audio %s ch/port %d-%d (slot %d)",
                 use_tcp ? "TCP" : "UDP", use_tcp ? rtp_ch : client_rtp_port,
                 use_tcp ? rtcp_ch : client_rtcp_port, slot);
    } else {
        session->use_tcp = use_tcp;
        if (use_tcp) {
            session->tcp_rtp_channel = rtp_ch;
            session->tcp_rtcp_channel = rtcp_ch;
        } else {
            session->client_rtp_port = client_rtp_port;
            session->client_rtcp_port = client_rtcp_port;

            if (session->udp_rtp_sock < 0) {
                session->udp_rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (session->udp_rtp_sock < 0) {
                    ESP_LOGE(TAG, "Failed to create video UDP socket: errno=%d", errno);
                    send_rtsp_response(sock, cseq, "500 Internal Server Error", NULL);
                    return false;
                }

                struct sockaddr_in local = {
                    .sin_family = AF_INET,
                    .sin_addr.s_addr = htonl(INADDR_ANY),
                    .sin_port = htons(0),
                };
                if (bind(session->udp_rtp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                    ESP_LOGE(TAG, "Video UDP bind failed: errno=%d", errno);
                    send_rtsp_response(sock, cseq, "500 Internal Server Error", NULL);
                    return false;
                }

                socklen_t local_len = sizeof(local);
                if (getsockname(session->udp_rtp_sock, (struct sockaddr *)&local, &local_len) == 0) {
                    session->server_rtp_port = ntohs(local.sin_port);
                    session->server_rtcp_port = session->server_rtp_port + 1;
                }
            }

            char client_ip[16] = {0};
            inet_ntop(AF_INET, &session->client_addr.sin_addr, client_ip, sizeof(client_ip));
            ESP_LOGI(TAG, "UDP video destination parsed: %s:%u rtcp=%u (server=%u-%u)",
                     client_ip,
                     (unsigned int)session->client_rtp_port,
                     (unsigned int)session->client_rtcp_port,
                     (unsigned int)session->server_rtp_port,
                     (unsigned int)session->server_rtcp_port);
        }
        ESP_LOGI(TAG, "SETUP video %s ch/port %d-%d (slot %d)",
                 use_tcp ? "TCP" : "UDP", use_tcp ? rtp_ch : client_rtp_port,
                 use_tcp ? rtcp_ch : client_rtcp_port, slot);
    }
    session->last_activity_ms = now_ms();

    // Send response
    char extra[256];
    if (use_tcp) {
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                 "Session: %08lx;timeout=60\r\n",
                 rtp_ch, rtcp_ch, (unsigned long)session->session_id);
    } else {
        uint16_t server_rtp_port = is_audio ? session->audio_server_rtp_port : session->server_rtp_port;
        uint16_t server_rtcp_port = is_audio ? session->audio_server_rtcp_port : session->server_rtcp_port;
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u\r\n"
                 "Session: %08lx;timeout=60\r\n",
                 client_rtp_port, client_rtcp_port,
                 (unsigned int)server_rtp_port, (unsigned int)server_rtcp_port,
                 (unsigned long)session->session_id);
    }
    send_rtsp_response(sock, cseq, "200 OK", extra);

    ESP_LOGI(TAG, "Session %08lx %s (slot %d)",
             (unsigned long)session->session_id,
             is_audio ? "audio track added" : "created",
             slot);
    return true;
}

static bool handle_play(int sock, int cseq, const char *req) {
    uint32_t requested_id = 0;
    if (!parse_session_id(req, &requested_id)) {
        ESP_LOGW(TAG, "PLAY rejected: missing/invalid Session header");
        send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
        return false;
    }

    int slot = find_slot_by_id(requested_id);
    if (slot < 0) {
        ESP_LOGW(TAG, "PLAY rejected: unknown session id %08lx", (unsigned long)requested_id);
        send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
        return false;
    }

    rtsp_session_t *session = sessions[slot];
    session->is_playing = true;
    session->last_frame_ms = now_ms();
    session->play_start_ms = session->last_frame_ms;
    session->audio_ts_initialized = false;
    session->last_audio_ms = session->last_frame_ms;
    session->last_activity_ms = session->last_frame_ms;

    char extra[384];
    int extra_len = snprintf(extra, sizeof(extra),
                             "Session: %08lx\r\n"
                             "RTP-Info: url=/mjpeg/1/track1;seq=%u;rtptime=%lu",
                             (unsigned long)session->session_id,
                             session->seq_num,
                             (unsigned long)session->timestamp);
    if (session->audio_setup && extra_len > 0 && extra_len < (int)sizeof(extra)) {
        extra_len += snprintf(extra + extra_len, sizeof(extra) - extra_len,
                              ",url=/mjpeg/1/track2;seq=%u;rtptime=%lu",
                              session->audio_seq_num,
                              (unsigned long)session->audio_timestamp);
    }
    if (extra_len > 0 && extra_len < (int)sizeof(extra)) {
        snprintf(extra + extra_len, sizeof(extra) - extra_len, "\r\n");
    }
    send_rtsp_response(sock, cseq, "200 OK", extra);

    ESP_LOGI(TAG, "PLAY session %08lx (audio_setup=%d use_tcp=%d audio_use_tcp=%d)",
             (unsigned long)session->session_id,
             session->audio_setup,
             session->use_tcp,
             session->audio_use_tcp);
    return true;
}

static void handle_teardown(int sock, int cseq, int slot) {
    if (slot >= 0 && sessions[slot]) {
        char extra[64];
        snprintf(extra, sizeof(extra), "Session: %08lx\r\n",
                 (unsigned long)sessions[slot]->session_id);
        send_rtsp_response(sock, cseq, "200 OK", extra);
        ESP_LOGI(TAG, "TEARDOWN session %08lx",
                 (unsigned long)sessions[slot]->session_id);
        // Don't close ctrl_sock here — the session owns it via cleanup
        sessions[slot]->ctrl_sock = -1; // Prevent double-close
        cleanup_session(slot, "teardown");
    } else {
        send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
    }
}

// ---------------------------------------------------------------------------
// RTP JPEG streaming
// ---------------------------------------------------------------------------

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

/**
 * Build RTP + JPEG header into buf (20 bytes).
 * Returns header size (always 20).
 */
static int build_rtp_jpeg_header(uint8_t *buf, rtsp_session_t *s,
                                  bool is_last, uint32_t frag_offset,
                                  uint8_t jpeg_type, uint8_t jpeg_q,
                                  uint16_t restart_interval,
                                  uint16_t width, uint16_t height,
                                  const uint8_t *qtables, uint16_t qtables_len) {
    // RTP header (12 bytes)
    buf[0] = 0x80;  // V=2, P=0, X=0, CC=0
    buf[1] = is_last ? 0x9A : 0x1A;  // M-bit | PT=26 (JPEG)
    put_be16(buf + 2, s->seq_num);
    put_be32(buf + 4, s->timestamp);
    put_be32(buf + 8, s->ssrc);

    // JPEG/RTP header (8 bytes, RFC 2435 §3.1)
    buf[12] = 0;  // Type-Specific
    buf[13] = (frag_offset >> 16) & 0xFF;
    buf[14] = (frag_offset >> 8) & 0xFF;
    buf[15] = frag_offset & 0xFF;
    buf[16] = jpeg_type;
    buf[17] = jpeg_q;
    buf[18] = width / 8;
    buf[19] = height / 8;

    int hdr_len = 20;
    if (restart_interval > 0) {
        // RFC 2435 §3.1.7 Restart Marker header (4 bytes)
        // F/L bits are keyed to frame fragment boundaries here.
        put_be16(buf + hdr_len, restart_interval);
        uint16_t restart_field = 0;
        if (frag_offset == 0) restart_field |= (1u << 15); // F
        if (is_last)          restart_field |= (1u << 14); // L
        put_be16(buf + hdr_len + 2, restart_field);        // Restart Count = 0
        hdr_len += 4;
    }

    if (frag_offset == 0 && jpeg_q >= 128 && qtables && qtables_len > 0) {
        // RFC 2435 §3.1.8 Quantization Table header (after optional restart header).
        buf[hdr_len + 0] = 0x00; // MBZ
        buf[hdr_len + 1] = 0x00; // Precision: 8-bit tables
        put_be16(buf + hdr_len + 2, qtables_len);
        memcpy(buf + hdr_len + 4, qtables, qtables_len);
        hdr_len += 4 + qtables_len;
    }

    return hdr_len;
}

static void send_rtp_jpeg_tcp(rtsp_session_t *s, camera_fb_t *fb) {
    if (!s || !fb || !s->is_playing || s->ctrl_sock < 0) return;

    uint8_t jpeg_type, jpeg_q;
    uint16_t restart_interval = 0;
    size_t scan_offset = find_jpeg_scan_data(fb->buf, fb->len, &jpeg_type, &jpeg_q,
                                             &restart_interval);
    if (!s->video_diag_logged) {
        ESP_LOGI(TAG, "JPEG parser: type=%u (ffmpeg: 0=4:2:2 1=4:2:0)", jpeg_type);
    }
#if RTP_JPEG_FORCE_TYPE_420
    jpeg_type = 1;
#endif
    // Legacy Arduino profile: force static Q so no explicit quantization tables are required.
    jpeg_q = 80;
    if (scan_offset == 0 || scan_offset >= fb->len) {
        ESP_LOGW(TAG,
                 "JPEG parse failed: sid=%08lx offset=%zu len=%zu head=%02x%02x",
                 (unsigned long)s->session_id,
                 scan_offset,
                 fb->len,
                 fb->len > 0 ? fb->buf[0] : 0,
                 fb->len > 1 ? fb->buf[1] : 0);
        if (!RTP_JPEG_PAYLOAD_FULL_FRAME && !RTP_JPEG_FORCE_FULL_ON_PARSE_FAIL) {
            return;
        }
    }

    size_t payload_offset = 0;
    size_t payload_len = fb->len;
    if (!RTP_JPEG_PAYLOAD_FULL_FRAME && !(scan_offset == 0 || scan_offset >= fb->len)) {
        payload_offset = scan_offset;
        payload_len = fb->len - scan_offset;
        if (!RTP_JPEG_INCLUDE_EOI && payload_len >= 2 &&
            fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
            payload_len -= 2;
        }
    }

    uint8_t jpeg_type_rtp = jpeg_type;
    if (restart_interval > 0) {
        jpeg_type_rtp |= 0x40;
    }

    const uint8_t *payload_data = fb->buf + payload_offset;
    size_t offset = 0;
    uint32_t frag_offset = 0;
#if RTP_JPEG_EXPLICIT_QTABLES
    jpeg_qtables_t frame_qtables;
    bool frame_has_qtables = extract_jpeg_qtables(fb->buf, fb->len, &frame_qtables);
    if (frame_has_qtables) {
        jpeg_q = 255;
    }
#endif

    // Fast path for TCP interleaved: send one RTP/JPEG packet per frame when possible.
    // This avoids fragmented partial-frame loss that can break strict decoders/probers.
    if (payload_len <= TCP_SINGLE_RTP_MAX) {
        size_t pkt_cap = 4 + 20 + 132 + payload_len;  // 132 = Q-table header (4) + tables (128)
        // Force internal DRAM: lwIP DMA cannot reliably read PSRAM-backed buffers.
        // At 15fps, 17KB/frame; SPIRAM_MALLOC_ALWAYSINTERNAL=16384 would push this to PSRAM.
        uint8_t *pkt_full = (uint8_t *)heap_caps_malloc(pkt_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (pkt_full) {
            uint16_t qtables_len = 0;
            const uint8_t *qtables_data = NULL;
#if RTP_JPEG_EXPLICIT_QTABLES
            if (frame_has_qtables) {
                qtables_len = frame_qtables.len;
                qtables_data = frame_qtables.data;
            }
#endif
            int jpeg_hdr_len = build_rtp_jpeg_header(
                pkt_full + 4,
                s,
                true,
                0,
                jpeg_type_rtp,
                jpeg_q,
                restart_interval,
                fb->width,
                fb->height,
                qtables_data,
                qtables_len
            );

            memcpy(pkt_full + 4 + jpeg_hdr_len, payload_data, payload_len);
            size_t rtp_len = (size_t)jpeg_hdr_len + payload_len;
            pkt_full[0] = '$';
            pkt_full[1] = s->tcp_rtp_channel;
            pkt_full[2] = (rtp_len >> 8) & 0xFF;
            pkt_full[3] = rtp_len & 0xFF;

            int send_result = send_packet_nonblock(s->ctrl_sock, pkt_full, 4 + rtp_len);
            free(pkt_full);

            if (send_result < 0) {
                ESP_LOGW(TAG, "TCP video write failed for session %08lx",
                         (unsigned long)s->session_id);
                int slot = find_slot_by_ptr(s);
                if (slot >= 0) cleanup_session(slot, "tcp_video_send_fail");
                return;
            }

            s->video_diag_logged = true;
            s->seq_num++;
            return;
        }
    }

    // Packet buffer: 4 (interleaved) + 20 (RTP+JPEG header) + payload
    uint8_t pkt[4 + 20 + MAX_RTP_PAYLOAD];

    while (offset < payload_len) {
        uint16_t qtables_len = 0;
        const uint8_t *qtables_data = NULL;
#if RTP_JPEG_EXPLICIT_QTABLES
        if (frame_has_qtables && frag_offset == 0) {
            qtables_len = frame_qtables.len;
            qtables_data = frame_qtables.data;
        }
#endif

        if (!s->video_diag_logged && frag_offset == 0) {
            ESP_LOGI(TAG,
                     "RTP/JPEG first frame (sid=%08lx): fb=%ux%u len=%u scan_off=%u payload=%u type=%u q=%u restart=%u qtables=%s eoi=%d",
                     (unsigned long)s->session_id,
                     (unsigned int)fb->width,
                     (unsigned int)fb->height,
                     (unsigned int)fb->len,
                     (unsigned int)scan_offset,
                     (unsigned int)payload_len,
                     (unsigned int)jpeg_type_rtp,
                     (unsigned int)jpeg_q,
                     (unsigned int)restart_interval,
#if RTP_JPEG_EXPLICIT_QTABLES
                     frame_has_qtables ? "yes" : "no",
#else
                     "off",
#endif
                     RTP_JPEG_INCLUDE_EOI ? 1 : 0);
            s->video_diag_logged = true;
        }
        int jpeg_hdr_len = build_rtp_jpeg_header(
            pkt + 4,
            s,
            false,
            frag_offset,
            jpeg_type_rtp,
            jpeg_q,
            restart_interval,
            fb->width,
            fb->height,
            qtables_data,
            qtables_len
        );

        size_t max_payload = (jpeg_hdr_len < (int)MAX_RTP_PAYLOAD)
            ? (MAX_RTP_PAYLOAD - (size_t)(jpeg_hdr_len - 20))
            : 1;

        size_t chunk = payload_len - offset;
        if (chunk > max_payload) chunk = max_payload;
        bool is_last = (offset + chunk >= payload_len);

        // Rebuild header with final marker bit
        jpeg_hdr_len = build_rtp_jpeg_header(
            pkt + 4,
            s,
            is_last,
            frag_offset,
            jpeg_type_rtp,
            jpeg_q,
            restart_interval,
            fb->width,
            fb->height,
            qtables_data,
            qtables_len
        );

        // Copy scan data
        memcpy(pkt + 4 + jpeg_hdr_len, payload_data + offset, chunk);

        // TCP interleaved header: $ + channel + length (2BE)
        size_t rtp_len = (size_t)jpeg_hdr_len + chunk;
        pkt[0] = '$';
        pkt[1] = s->tcp_rtp_channel;
        pkt[2] = (rtp_len >> 8) & 0xFF;
        pkt[3] = rtp_len & 0xFF;

        int send_result = send_packet_nonblock(s->ctrl_sock, pkt, 4 + rtp_len);
        if (send_result < 0) {
            ESP_LOGW(TAG, "TCP video write failed for session %08lx",
                     (unsigned long)s->session_id);
            int slot = find_slot_by_ptr(s);
            if (slot >= 0) cleanup_session(slot, "tcp_video_send_fail");
            return;
        }

        s->seq_num++;
        offset += chunk;
        frag_offset += chunk;
    }
}

static void apply_udp_backoff(rtsp_session_t *s) {
    if (s->udp_fail_streak < 10) s->udp_fail_streak++;
    uint32_t backoff = UDP_BACKOFF_BASE_MS * s->udp_fail_streak;
    if (backoff > UDP_BACKOFF_MAX_MS) backoff = UDP_BACKOFF_MAX_MS;
    s->udp_backoff_until_ms = now_ms() + backoff;
    g_udp_fail_total++;
}

static void send_rtp_jpeg_udp(rtsp_session_t *s, camera_fb_t *fb) {
    if (!s || !fb || !s->is_playing) return;
    if (now_ms() < s->udp_backoff_until_ms) return;

    // Lazy-init UDP socket
    if (s->udp_rtp_sock < 0) {
        s->udp_rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s->udp_rtp_sock < 0) {
            ESP_LOGE(TAG, "Failed to create UDP socket");
            return;
        }
        // Allow brief wait for LWIP pbufs instead of instant ENOMEM
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 }; // 10ms
        setsockopt(s->udp_rtp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    uint8_t jpeg_type, jpeg_q;
    uint16_t restart_interval = 0;
    size_t scan_offset = find_jpeg_scan_data(fb->buf, fb->len, &jpeg_type, &jpeg_q,
                                             &restart_interval);
#if RTP_JPEG_FORCE_TYPE_420
    jpeg_type = 1;
#endif
    // Legacy Arduino profile: force static Q so no explicit quantization tables are required.
    jpeg_q = 80;
    if (scan_offset == 0 || scan_offset >= fb->len) {
        ESP_LOGW(TAG,
                 "UDP: JPEG parse failed: sid=%08lx offset=%zu len=%zu head=%02x%02x",
                 (unsigned long)s->session_id,
                 scan_offset,
                 fb->len,
                 fb->len > 0 ? fb->buf[0] : 0,
                 fb->len > 1 ? fb->buf[1] : 0);
        if (!RTP_JPEG_PAYLOAD_FULL_FRAME && !RTP_JPEG_FORCE_FULL_ON_PARSE_FAIL) {
            return;
        }
    }

    size_t payload_offset = 0;
    size_t payload_len = fb->len;
    if (!RTP_JPEG_PAYLOAD_FULL_FRAME && !(scan_offset == 0 || scan_offset >= fb->len)) {
        payload_offset = scan_offset;
        payload_len = fb->len - scan_offset;
        if (!RTP_JPEG_INCLUDE_EOI && payload_len >= 2 &&
            fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
            payload_len -= 2;
        }
    }

    uint8_t jpeg_type_rtp = jpeg_type;
    if (restart_interval > 0) {
        jpeg_type_rtp |= 0x40;
    }

    const uint8_t *payload_data = fb->buf + payload_offset;
    size_t offset = 0;
    uint32_t frag_offset = 0;
#if RTP_JPEG_EXPLICIT_QTABLES
    jpeg_qtables_t frame_qtables;
    bool frame_has_qtables = extract_jpeg_qtables(fb->buf, fb->len, &frame_qtables);
    if (frame_has_qtables) {
        jpeg_q = 255;
    }
#endif

    struct sockaddr_in dest = s->client_addr;
    dest.sin_port = htons(s->client_rtp_port);

    // Log first frame destination once per session
    if (s->seq_num == 0) {
        char dest_ip[16];
        inet_ntop(AF_INET, &dest.sin_addr, dest_ip, sizeof(dest_ip));
        ESP_LOGI(TAG, "UDP: sending first frame %zu bytes payload to %s:%d",
             payload_len, dest_ip, s->client_rtp_port);
    }

    uint8_t pkt[20 + 4 + 128 + MAX_RTP_PAYLOAD];

    while (offset < payload_len) {
        uint16_t qtables_len = 0;
        const uint8_t *qtables_data = NULL;
#if RTP_JPEG_EXPLICIT_QTABLES
        if (frame_has_qtables && frag_offset == 0) {
            qtables_len = frame_qtables.len;
            qtables_data = frame_qtables.data;
        }
#endif

        if (!s->video_diag_logged && frag_offset == 0) {
            ESP_LOGI(TAG,
                     "RTP/JPEG first frame (sid=%08lx UDP): fb=%ux%u len=%u scan_off=%u payload=%u type=%u q=%u restart=%u qtables=%s eoi=%d",
                     (unsigned long)s->session_id,
                     (unsigned int)fb->width,
                     (unsigned int)fb->height,
                     (unsigned int)fb->len,
                     (unsigned int)scan_offset,
                     (unsigned int)payload_len,
                     (unsigned int)jpeg_type_rtp,
                     (unsigned int)jpeg_q,
                     (unsigned int)restart_interval,
#if RTP_JPEG_EXPLICIT_QTABLES
                     frame_has_qtables ? "yes" : "no",
#else
                     "off",
#endif
                     RTP_JPEG_INCLUDE_EOI ? 1 : 0);
            s->video_diag_logged = true;
        }
        int jpeg_hdr_len = build_rtp_jpeg_header(
            pkt,
            s,
            false,
            frag_offset,
            jpeg_type_rtp,
            jpeg_q,
            restart_interval,
            fb->width,
            fb->height,
            qtables_data,
            qtables_len
        );

        size_t max_payload = (jpeg_hdr_len < (int)MAX_RTP_PAYLOAD)
            ? (MAX_RTP_PAYLOAD - (size_t)(jpeg_hdr_len - 20))
            : 1;

        size_t chunk = payload_len - offset;
        if (chunk > max_payload) chunk = max_payload;
        bool is_last = (offset + chunk >= payload_len);

        jpeg_hdr_len = build_rtp_jpeg_header(
            pkt,
            s,
            is_last,
            frag_offset,
            jpeg_type_rtp,
            jpeg_q,
            restart_interval,
            fb->width,
            fb->height,
            qtables_data,
            qtables_len
        );
        memcpy(pkt + jpeg_hdr_len, payload_data + offset, chunk);

        size_t pkt_len = (size_t)jpeg_hdr_len + chunk;
        int sent = sendto(s->udp_rtp_sock, pkt, pkt_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
        if (sent < 0 || (size_t)sent != pkt_len) {
            int err = errno;
            g_udp_fail_total++;
            if (err == ENOMEM || err == EAGAIN || err == EWOULDBLOCK) {
                // Transient LWIP pbuf exhaustion: skip this fragment, continue
                // sending remaining fragments (decoder tolerates partial frames
                // better than multi-frame blackouts from backoff).
                s->seq_num++;
                offset += chunk;
                frag_offset += chunk;
                continue;
            }
            // Persistent error (EHOSTUNREACH, ECONNREFUSED, etc): back off
            apply_udp_backoff(s);
            char dest_ip[16] = {0};
            inet_ntop(AF_INET, &dest.sin_addr, dest_ip, sizeof(dest_ip));
            ESP_LOGW(TAG, "UDP video send failed: dest=%s:%u sent=%d expected=%u errno=%d",
                     dest_ip,
                     (unsigned int)ntohs(dest.sin_port),
                     sent,
                     (unsigned int)pkt_len,
                     err);
            return;
        }

        s->seq_num++;
        offset += chunk;
        frag_offset += chunk;

        // Pace UDP packets to prevent buffer overflow
        if (offset < payload_len) {
            vTaskDelay(1); // ~1ms between fragments
        }
    }
    s->udp_fail_streak = 0;
}

static void send_rtp_jpeg(rtsp_session_t *s, camera_fb_t *fb) {
    if (s->use_tcp) {
        send_rtp_jpeg_tcp(s, fb);
    } else {
        send_rtp_jpeg_udp(s, fb);
    }
}

// ---------------------------------------------------------------------------
// RTP AAC-hbr streaming (RFC 3640)
// ---------------------------------------------------------------------------

static void send_rtp_aac_tcp(rtsp_session_t *s, const uint8_t *aac, size_t aac_len) {
    if (!s || !s->is_playing || !s->audio_setup || s->ctrl_sock < 0) return;

    // 4 (interleaved) + 12 (RTP) + 4 (AU header section) + payload
    uint8_t pkt[4 + 12 + 4 + 2048];
    uint8_t *rtp = pkt + 4;

    // RTP header (12 bytes)
    rtp[0] = 0x80;                              // V=2
    rtp[1] = 0x80 | 96;                         // M=1, PT=96 (0x80=M bit; 96=0x60 so result=0xE0)
    put_be16(rtp + 2, s->audio_seq_num);
    put_be32(rtp + 4, s->audio_timestamp);
    put_be32(rtp + 8, s->audio_ssrc);

    // AU-headers-length: 16 bits (1 AU header × 16 bits = 0x0010)
    rtp[12] = 0x00;
    rtp[13] = 0x10;
    // AU-header: 13-bit size + 3-bit index
    uint16_t au_header = (uint16_t)((aac_len << 3) & 0xFFF8);
    rtp[14] = (au_header >> 8) & 0xFF;
    rtp[15] = au_header & 0xFF;

    memcpy(rtp + 16, aac, aac_len);
    size_t rtp_len = 16 + aac_len;

    // TCP interleaved framing
    pkt[0] = '$';
    pkt[1] = s->audio_tcp_rtp_channel;
    pkt[2] = (rtp_len >> 8) & 0xFF;
    pkt[3] = rtp_len & 0xFF;

    int send_result = send_packet_nonblock(s->ctrl_sock, pkt, 4 + rtp_len);
    if (send_result < 0) {
        ESP_LOGW(TAG, "Audio TCP write failed for session %08lx",
                 (unsigned long)s->session_id);
        int slot = find_slot_by_ptr(s);
        if (slot >= 0) cleanup_session(slot, "tcp_audio_send_fail");
    }
}

static void send_rtp_aac_udp(rtsp_session_t *s, const uint8_t *aac, size_t aac_len) {
    if (!s || !s->is_playing || !s->audio_setup) return;
    if (now_ms() < s->udp_backoff_until_ms) return;
    if (s->audio_client_rtp_port == 0) return;

    // Lazy-init UDP socket
    if (s->udp_audio_rtp_sock < 0) {
        s->udp_audio_rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s->udp_audio_rtp_sock < 0) {
            ESP_LOGE(TAG, "Failed to create audio UDP socket");
            return;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 }; // 10ms
        setsockopt(s->udp_audio_rtp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    uint8_t pkt[12 + 4 + 2048];

    // RTP header (12 bytes)
    pkt[0] = 0x80;
    pkt[1] = 0x80 | 96;                         // M=1, PT=96 (0x80=M bit; 96=0x60 so result=0xE0)
    put_be16(pkt + 2, s->audio_seq_num);
    put_be32(pkt + 4, s->audio_timestamp);
    put_be32(pkt + 8, s->audio_ssrc);

    // AU-headers-length + AU-header
    pkt[12] = 0x00;
    pkt[13] = 0x10;
    uint16_t au_header = (uint16_t)((aac_len << 3) & 0xFFF8);
    pkt[14] = (au_header >> 8) & 0xFF;
    pkt[15] = au_header & 0xFF;

    memcpy(pkt + 16, aac, aac_len);
    size_t pkt_len = 16 + aac_len;

    struct sockaddr_in dest = s->client_addr;
    dest.sin_port = htons(s->audio_client_rtp_port);

    if (s->audio_seq_num == 0) {
        char dest_ip[16] = {0};
        inet_ntop(AF_INET, &dest.sin_addr, dest_ip, sizeof(dest_ip));
        ESP_LOGI(TAG, "UDP: sending first AAC packet %zu bytes to %s:%u",
                 pkt_len,
                 dest_ip,
                 (unsigned int)s->audio_client_rtp_port);
    }

    int sent = sendto(s->udp_audio_rtp_sock, pkt, pkt_len, 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0 || (size_t)sent != pkt_len) {
        int err = errno;
        g_udp_fail_total++;
        if (err != ENOMEM && err != EAGAIN && err != EWOULDBLOCK) {
            // Persistent error: apply backoff
            apply_udp_backoff(s);
            char dest_ip[16] = {0};
            inet_ntop(AF_INET, &dest.sin_addr, dest_ip, sizeof(dest_ip));
            ESP_LOGW(TAG, "UDP audio send failed: dest=%s:%u sent=%d expected=%u errno=%d",
                     dest_ip,
                     (unsigned int)ntohs(dest.sin_port),
                     sent,
                     (unsigned int)pkt_len,
                     err);
        }
        // Transient (ENOMEM/EAGAIN): silently drop this audio packet, no backoff
    }
}

static void send_rtp_aac(rtsp_session_t *s, const uint8_t *aac, size_t aac_len) {
    if (!RTSP_SEND_AUDIO_RTP) {
        if (s && !s->audio_tx_disabled_logged) {
            ESP_LOGW(TAG, "Audio RTP TX disabled for debug (sid=%08lx)",
                     (unsigned long)s->session_id);
            s->audio_tx_disabled_logged = true;
        }
        return;
    }

    if (s->audio_use_tcp) {
        send_rtp_aac_tcp(s, aac, aac_len);
    } else {
        send_rtp_aac_udp(s, aac, aac_len);
    }
    if (!s->audio_ts_initialized && s->play_start_ms > 0) {
        uint32_t elapsed_ms = now_ms() - s->play_start_ms;
        s->audio_timestamp = elapsed_ms * 16;  // 16000 Hz / 1000 ms
        s->audio_ts_initialized = true;
    }
    s->audio_seq_num++;
    s->audio_timestamp += AAC_FRAME_SAMPLES;
}

// ---------------------------------------------------------------------------
// Get local IP for SDP
// ---------------------------------------------------------------------------
static void get_local_ip(char *ip_buf, size_t size) {
    // Use esp_netif to get the WiFi station IP — reliable regardless of routing table state
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_buf, size);
            ESP_LOGI(TAG, "Local IP (netif): %s", ip_buf);
            return;
        }
    }
    ESP_LOGW(TAG, "get_local_ip: netif unavailable, falling back to 0.0.0.0");
    strncpy(ip_buf, "0.0.0.0", size);
}

// ---------------------------------------------------------------------------
// RTSP server task
// ---------------------------------------------------------------------------

static void rtsp_server_task(void *pvParameters) {
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(RTSP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(listen_sock);
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 2) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(listen_sock);
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Get local IP for SDP
    char local_ip[16];
    get_local_ip(local_ip, sizeof(local_ip));
    ESP_LOGI(TAG, "RTSP server listening on port %d (IP: %s)", RTSP_PORT, local_ip);

    // Make accept non-blocking so the streaming loop is not gated on new connections.
    // accept() returns EAGAIN immediately when no client is waiting.
    int lflags = fcntl(listen_sock, F_GETFL, 0);
    fcntl(listen_sock, F_SETFL, lflags | O_NONBLOCK);
    uint32_t last_fd_pressure_log_ms = 0;

    // AAC pipeline is started eagerly by aac_encoder_pipe_init() at boot,
    // so no pre-init call is needed here.

    while (server_running) {
        vTaskDelay(pdMS_TO_TICKS(5)); // brief yield; accept() returns EAGAIN instantly when idle
        reap_dead_sessions();

        // ----- Accept new RTSP control connections -----
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);

        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Accept timeout: normal periodic wakeup.
            } else if (errno == EMFILE || errno == ENFILE) {
                uint32_t cur_ms = now_ms();
                if (cur_ms - last_fd_pressure_log_ms > 2000) {
                    last_fd_pressure_log_ms = cur_ms;
                    ESP_LOGW(TAG, "accept() fd pressure: errno=%d active_sessions=%d", errno,
                             rtsp_server_active_session_count());
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            }
        }

        if (client_sock >= 0) {
            char ip_str[16];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Client connected from %s", ip_str);

            // TCP_NODELAY for low latency
            int flag = 1;
            setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            // Send timeout
            struct timeval tv = {
                .tv_sec = TCP_SEND_TIMEOUT_MS / 1000,
                .tv_usec = (TCP_SEND_TIMEOUT_MS % 1000) * 1000,
            };
            setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            // Low-latency socket tuning: reduce kernel send buffer and tighten timeout
            if (s_low_latency_mode) {
                int sndbuf = 2048;
                setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                struct timeval tv_tight = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_tight, sizeof(tv_tight));
            }

            // RTSP handshake (blocking until PLAY or disconnect)
            bool session_started = false;
            int setup_slot = -1;
            char req_buf[REQ_BUF_SIZE];

            while (server_running) {
                int n = recv_request(client_sock, req_buf, sizeof(req_buf),
                                     HANDSHAKE_TIMEOUT_S);
                if (n <= 0) break;

                char method[16];
                parse_method(req_buf, method, sizeof(method));
                int cseq = parse_cseq(req_buf);

                ESP_LOGD(TAG, "RTSP %s (CSeq=%d)", method, cseq);

                if (strcmp(method, "OPTIONS") == 0) {
                    handle_options(client_sock, cseq);
                } else if (!rtsp_check_auth(req_buf)) {
                    // Send 401 — client will retry with credentials in same connection
                    send_rtsp_response(client_sock, cseq, "401 Unauthorized",
                        "WWW-Authenticate: Basic realm=\"ESP32-S3 Camera\"\r\n");
                } else if (strcmp(method, "DESCRIBE") == 0) {
                    handle_describe(client_sock, cseq, local_ip);
                } else if (strcmp(method, "SETUP") == 0) {
                    char transport[256];
                    if (parse_transport(req_buf, transport, sizeof(transport))) {
                        handle_setup(client_sock, cseq, req_buf, transport,
                                     &client_addr);
                        setup_slot = find_slot_by_sock(client_sock);
                    } else {
                        send_rtsp_response(client_sock, cseq,
                                           "461 Unsupported Transport", NULL);
                    }
                } else if (strcmp(method, "PLAY") == 0) {
                    if (handle_play(client_sock, cseq, req_buf)) {
                        session_started = true;
                        break;
                    }
                } else if (strcmp(method, "TEARDOWN") == 0) {
                    int slot = find_slot_by_sock(client_sock);
                    handle_teardown(client_sock, cseq, slot);
                    client_sock = -1; // Ownership transferred
                    break;
                } else {
                    send_rtsp_response(client_sock, cseq,
                                       "501 Not Implemented", NULL);
                    break;
                }
            }

            if (!session_started) {
                if (setup_slot >= 0 && sessions[setup_slot]) {
                    ESP_LOGI(TAG, "Client disconnected during handshake");
                    cleanup_session(setup_slot, "handshake_disconnect");
                }
                if (client_sock >= 0) {
                    close(client_sock);
                }
            }
        }

        // ----- Check for TEARDOWN on active sessions (non-blocking) -----
        uint32_t cur = now_ms();
        for (int i = 0; i < MAX_SESSIONS; i++) {
            rtsp_session_t *s = sessions[i];
            if (!s || !s->is_playing) continue;

            // Check for incoming data (TEARDOWN / keepalive)
            char req_buf2[REQ_BUF_SIZE];
            int n = recv_nonblock(s->ctrl_sock, req_buf2, sizeof(req_buf2));
            if (n > 0) {
                char method2[16];
                parse_method(req_buf2, method2, sizeof(method2));
                int cseq2 = parse_cseq(req_buf2);
                ESP_LOGD(TAG, "Session %08lx: %s (CSeq=%d)",
                         (unsigned long)s->session_id, method2, cseq2);
                if (strcmp(method2, "TEARDOWN") == 0) {
                    handle_teardown(s->ctrl_sock, cseq2, i);
                    continue;
                } else if (strcmp(method2, "OPTIONS") == 0) {
                    handle_options(s->ctrl_sock, cseq2);
                } else if (strcmp(method2, "GET_PARAMETER") == 0) {
                    send_rtsp_response(s->ctrl_sock, cseq2, "200 OK", NULL);
                }
                s->last_activity_ms = cur;
            } else if (n < 0) {
                // Connection closed
                ESP_LOGI(TAG, "Client disconnected: session %08lx",
                         (unsigned long)s->session_id);
                cleanup_session(i, "recv_nonblock_closed");
                continue;
            }

            // Session timeout
            if (cur - s->last_activity_ms > SESSION_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Session timeout: %08lx",
                         (unsigned long)s->session_id);
                cleanup_session(i, "session_timeout");
                continue;
            }
        }

        // Proactively free sockets from dead sessions (ctrl_sock == -1)
        reap_dead_sessions();

        // ----- Stream frames to active sessions -----
        cur = now_ms();
        bool any_needs_frame = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            rtsp_session_t *s = sessions[i];
            if (s && s->is_playing && (cur - s->last_frame_ms >= FRAME_INTERVAL_MS)) {
                any_needs_frame = true;
                break;
            }
        }

        if (any_needs_frame) {
            captured_frame_t frame;
            if (!camera_capture_frame(&frame)) {
                ESP_LOGW(TAG, "camera_capture returned NULL - no frame");
            } else {
                last_frame_width = frame.width;
                last_frame_height = frame.height;

                // Create a local camera_fb_t shim for send_rtp_jpeg compatibility
                camera_fb_t fb_shim = {
                    .buf = (uint8_t *)frame.buf,
                    .len = frame.len,
                    .width = frame.width,
                    .height = frame.height,
                    .format = PIXFORMAT_JPEG,
                };

                for (int i = 0; i < MAX_SESSIONS; i++) {
                    rtsp_session_t *s = sessions[i];
                    if (!s || !s->is_playing) continue;
                    if (cur - s->last_frame_ms < FRAME_INTERVAL_MS) continue;

                    if (!s->video_send_diag_logged && s->seq_num == 0) {
                        ESP_LOGI(TAG,
                                 "First video send attempt sid=%08lx elapsed=%lu use_tcp=%d",
                                 (unsigned long)s->session_id,
                                 (unsigned long)(cur - s->last_frame_ms),
                                 s->use_tcp);
                        s->video_send_diag_logged = true;
                    }

                    uint16_t seq_before = s->seq_num;
                    send_rtp_jpeg(s, &fb_shim);
                    if (seq_before == s->seq_num) {
                        ESP_LOGW(TAG,
                                 "No video RTP emitted sid=%08lx (seq unchanged=%u)",
                                 (unsigned long)s->session_id,
                                 (unsigned int)s->seq_num);
                    }

                    if (s->last_frame_ms > 0) {
                        uint32_t delta = cur - s->last_frame_ms;
                        uint32_t increment = delta * 90; // 90kHz clock
                        if (increment == 0) increment = 1;
                        s->timestamp += increment;
                    }
                    s->last_frame_ms = cur;
                    s->last_activity_ms = cur;
                }
                camera_release_frame(&frame);
            }
        }

        // ----- Stream audio to sessions with audio track setup -----
        {
            uint32_t audio_rate = aac_encoder_pipe_get_sample_rate();
            uint32_t audio_interval = (audio_rate > 0)
                ? (AAC_FRAME_SAMPLES * 1000 / audio_rate) : 64;
            if (audio_interval < 20) audio_interval = 20;

            bool any_needs_audio = false;
            for (int i = 0; i < MAX_SESSIONS; i++) {
                rtsp_session_t *s = sessions[i];
                if (s && s->is_playing && s->audio_setup &&
                    (cur - s->last_audio_ms >= audio_interval)) {
                    any_needs_audio = true;
                    break;
                }
            }

            if (any_needs_audio) {
                static uint8_t aac_buf[2048];
                size_t aac_len = 0;
                bool got_frame = aac_encoder_pipe_get_frame(aac_buf, sizeof(aac_buf), &aac_len)
                                 && aac_len > 0;

                // Always advance last_audio_ms regardless of whether we got a frame.
                // If a frame fails and we skip updating it, every loop iteration
                // re-evaluates any_needs_audio=true and hammers get_frame, blocking
                // the RTSP task and starving video (fd=0).
                for (int i = 0; i < MAX_SESSIONS; i++) {
                    rtsp_session_t *s = sessions[i];
                    if (!s || !s->is_playing || !s->audio_setup) continue;
                    if (cur - s->last_audio_ms < audio_interval) continue;

                    if (got_frame) {
                        send_rtp_aac(s, aac_buf, aac_len);
                        s->last_activity_ms = cur;
                    }
                    s->last_audio_ms = cur;
                }
            }
        }

        // Yield
        vTaskDelay(1);
    }

    // Cleanup all sessions
    for (int i = 0; i < MAX_SESSIONS; i++) {
        cleanup_session(i, "server_stop");
    }
    close(listen_sock);
    ESP_LOGI(TAG, "RTSP server stopped");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t rtsp_server_start(void) {
    if (server_running) {
        ESP_LOGW(TAG, "RTSP server already running");
        return ESP_OK;
    }

    if (!camera_is_ready()) {
        ESP_LOGE(TAG, "Cannot start RTSP server: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }

    server_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        rtsp_server_task,
        "rtsp_server",
        SERVER_TASK_STACK,
        NULL,
        1,
        &server_task_handle,
        SERVER_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RTSP server task");
        server_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RTSP server started on port %d", RTSP_PORT);
    return ESP_OK;
}

void rtsp_server_stop(void) {
    if (!server_running) return;

    ESP_LOGI(TAG, "Stopping RTSP server...");
    server_running = false;

    // Wait for task to exit
    int timeout = 30;
    while (server_task_handle && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
    server_task_handle = NULL;
}

bool rtsp_server_is_running(void) {
    return server_running;
}

int rtsp_server_active_session_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i] && sessions[i]->is_playing) count++;
    }
    return count;
}

void rtsp_server_get_client_ips(char out[][16], int max, int *count) {
    *count = 0;
    for (int i = 0; i < MAX_SESSIONS && *count < max; i++) {
        if (sessions[i] && sessions[i]->is_playing) {
            inet_ntop(AF_INET, &sessions[i]->client_addr.sin_addr,
                      out[*count], 16);
            (*count)++;
        }
    }
}

int rtsp_server_udp_fail_count(void) {
    return (int)g_udp_fail_total;
}

void rtsp_server_udp_fail_reset(void) {
    g_udp_fail_total = 0;
}

uint32_t rtsp_server_udp_backoff_max_ms(void) {
    uint32_t cur = now_ms();
    uint32_t max_remaining = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i] && sessions[i]->is_playing && sessions[i]->udp_backoff_until_ms > cur) {
            uint32_t remaining = sessions[i]->udp_backoff_until_ms - cur;
            if (remaining > max_remaining) max_remaining = remaining;
        }
    }
    return max_remaining;
}

void rtsp_server_set_allow_udp(bool udp_allowed) {
    allow_udp = udp_allowed;
}

void rtsp_server_set_low_latency(bool enabled) {
    s_low_latency_mode = enabled;
}

void rtsp_server_set_credentials(const char *user, const char *pass) {
    if (user) {
        strncpy(rtsp_auth_user, user, sizeof(rtsp_auth_user) - 1);
        rtsp_auth_user[sizeof(rtsp_auth_user) - 1] = '\0';
    } else {
        rtsp_auth_user[0] = '\0';
    }
    if (pass) {
        strncpy(rtsp_auth_pass, pass, sizeof(rtsp_auth_pass) - 1);
        rtsp_auth_pass[sizeof(rtsp_auth_pass) - 1] = '\0';
    } else {
        rtsp_auth_pass[0] = '\0';
    }
    ESP_LOGI(TAG, "RTSP auth %s", rtsp_auth_user[0] ? "enabled" : "disabled (open)");
}

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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

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
    uint16_t audio_seq_num;
    uint32_t audio_timestamp;
    uint32_t audio_ssrc;
    uint32_t last_audio_ms;

    // UDP backoff
    uint32_t udp_backoff_until_ms;
    uint8_t udp_fail_streak;
} rtsp_session_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static volatile bool server_running = false;
static rtsp_session_t *sessions[MAX_SESSIONS] = {NULL};
static TaskHandle_t server_task_handle = NULL;
static bool allow_udp = false;
static uint16_t last_frame_width = 0;
static uint16_t last_frame_height = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; // 50ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = recv(sock, buf, buf_size - 1, 0);
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

static void cleanup_session(int slot) {
    if (slot < 0 || slot >= MAX_SESSIONS || !sessions[slot]) return;
    rtsp_session_t *s = sessions[slot];

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

// ---------------------------------------------------------------------------
// JPEG scan data parser (RFC 2435)
// ---------------------------------------------------------------------------

/**
 * Parse JPEG to find entropy-coded scan data offset.
 * Sets type (0=4:2:0, 1=4:2:2) and returns offset to scan data.
 * Returns 0 if JPEG is invalid or SOS not found.
 */
static size_t find_jpeg_scan_data(const uint8_t *jpeg, size_t len,
                                   uint8_t *type, uint8_t *q) {
    *type = 0;  // Default: YUV 4:2:0
    *q = 80;    // Fixed Q (avoids needing Q-table header for Q >= 128)

    if (len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return 0;
    }

    size_t i = 2;
    while (i < len - 1) {
        if (jpeg[i] != 0xFF) return 0;

        uint8_t marker = jpeg[i + 1];
        i += 2;

        // SOF0: determine chroma subsampling
        if (marker == 0xC0) {
            if (i + 7 > len) return 0;
            if (i + 6 < len) {
                uint8_t y_sampling = jpeg[i + 6];
                if (y_sampling == 0x21) {
                    *type = 1; // 4:2:2
                } else if (y_sampling == 0x22) {
                    *type = 0; // 4:2:0
                }
            }
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

    if (last_frame_width > 0 && last_frame_height > 0) {
        sdp_len = snprintf(sdp, sizeof(sdp),
            "v=0\r\n"
            "o=- 0 0 IN IP4 %s\r\n"
            "s=ESP32-S3 Camera\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=control:rtsp://%s:%d/mjpeg/1\r\n"
            "m=video 0 RTP/AVP 26\r\n"
            "a=rtpmap:26 JPEG/90000\r\n"
            "a=framesize:26 %u-%u\r\n"
            "a=control:rtsp://%s:%d/mjpeg/1/track1\r\n",
            local_ip, local_ip, RTSP_PORT,
            last_frame_width, last_frame_height,
            local_ip, RTSP_PORT);
    } else {
        sdp_len = snprintf(sdp, sizeof(sdp),
            "v=0\r\n"
            "o=- 0 0 IN IP4 %s\r\n"
            "s=ESP32-S3 Camera\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=control:rtsp://%s:%d/mjpeg/1\r\n"
            "m=video 0 RTP/AVP 26\r\n"
            "a=rtpmap:26 JPEG/90000\r\n"
            "a=control:rtsp://%s:%d/mjpeg/1/track1\r\n",
            local_ip, local_ip, RTSP_PORT,
            local_ip, RTSP_PORT);
    }

    // Append audio SDP track when mic is enabled
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
    if (is_audio && !audio_capture_is_enabled()) {
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
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n"
                 "Session: %08lx;timeout=60\r\n",
                 client_rtp_port, client_rtcp_port,
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
        send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
        return false;
    }

    int slot = find_slot_by_id(requested_id);
    if (slot < 0) {
        send_rtsp_response(sock, cseq, "454 Session Not Found", NULL);
        return false;
    }

    rtsp_session_t *session = sessions[slot];
    session->is_playing = true;
    session->last_frame_ms = now_ms();
    session->last_activity_ms = now_ms();

    char extra[64];
    snprintf(extra, sizeof(extra), "Session: %08lx\r\n",
             (unsigned long)session->session_id);
    send_rtsp_response(sock, cseq, "200 OK", extra);

    ESP_LOGI(TAG, "PLAY session %08lx", (unsigned long)session->session_id);
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
        cleanup_session(slot);
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
                                  uint16_t width, uint16_t height) {
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

    return 20;
}

static void send_rtp_jpeg_tcp(rtsp_session_t *s, camera_fb_t *fb) {
    if (!s || !fb || !s->is_playing || s->ctrl_sock < 0) return;

    uint8_t jpeg_type, jpeg_q;
    size_t scan_offset = find_jpeg_scan_data(fb->buf, fb->len, &jpeg_type, &jpeg_q);
    if (scan_offset == 0 || scan_offset >= fb->len) {
        ESP_LOGW(TAG, "JPEG parse failed: offset=%zu len=%zu", scan_offset, fb->len);
        return;
    }

    // Strip EOI marker at end
    size_t scan_len = fb->len - scan_offset;
    if (scan_len >= 2 &&
        fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
        scan_len -= 2;
    }

    const uint8_t *scan_data = fb->buf + scan_offset;
    size_t offset = 0;
    uint32_t frag_offset = 0;

    // Packet buffer: 4 (interleaved) + 20 (RTP+JPEG header) + payload
    uint8_t pkt[4 + 20 + MAX_RTP_PAYLOAD];

    while (offset < scan_len) {
        size_t chunk = scan_len - offset;
        if (chunk > MAX_RTP_PAYLOAD) chunk = MAX_RTP_PAYLOAD;
        bool is_last = (offset + chunk >= scan_len);

        // Build RTP + JPEG header at pkt[4]
        build_rtp_jpeg_header(pkt + 4, s, is_last, frag_offset,
                              jpeg_type, jpeg_q, fb->width, fb->height);

        // Copy scan data
        memcpy(pkt + 4 + 20, scan_data + offset, chunk);

        // TCP interleaved header: $ + channel + length (2BE)
        size_t rtp_len = 20 + chunk;
        pkt[0] = '$';
        pkt[1] = s->tcp_rtp_channel;
        pkt[2] = (rtp_len >> 8) & 0xFF;
        pkt[3] = rtp_len & 0xFF;

        if (send_all(s->ctrl_sock, pkt, 4 + rtp_len) < 0) {
            ESP_LOGW(TAG, "TCP write failed for session %08lx",
                     (unsigned long)s->session_id);
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
    }

    uint8_t jpeg_type, jpeg_q;
    size_t scan_offset = find_jpeg_scan_data(fb->buf, fb->len, &jpeg_type, &jpeg_q);
    if (scan_offset == 0 || scan_offset >= fb->len) return;

    size_t scan_len = fb->len - scan_offset;
    if (scan_len >= 2 &&
        fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9) {
        scan_len -= 2;
    }

    const uint8_t *scan_data = fb->buf + scan_offset;
    size_t offset = 0;
    uint32_t frag_offset = 0;

    struct sockaddr_in dest = s->client_addr;
    dest.sin_port = htons(s->client_rtp_port);

    uint8_t pkt[20 + MAX_RTP_PAYLOAD];

    while (offset < scan_len) {
        size_t chunk = scan_len - offset;
        if (chunk > MAX_RTP_PAYLOAD) chunk = MAX_RTP_PAYLOAD;
        bool is_last = (offset + chunk >= scan_len);

        build_rtp_jpeg_header(pkt, s, is_last, frag_offset,
                              jpeg_type, jpeg_q, fb->width, fb->height);
        memcpy(pkt + 20, scan_data + offset, chunk);

        size_t pkt_len = 20 + chunk;
        int sent = sendto(s->udp_rtp_sock, pkt, pkt_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
        if (sent < 0 || (size_t)sent != pkt_len) {
            apply_udp_backoff(s);
            ESP_LOGW(TAG, "UDP send failed");
            return;
        }

        s->seq_num++;
        offset += chunk;
        frag_offset += chunk;

        // Pace UDP packets to prevent buffer overflow
        if (offset < scan_len) {
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
    rtp[1] = 0x60 | 96;                         // M=1, PT=96
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

    if (send_all(s->ctrl_sock, pkt, 4 + rtp_len) < 0) {
        ESP_LOGW(TAG, "Audio TCP write failed for session %08lx",
                 (unsigned long)s->session_id);
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
    }

    uint8_t pkt[12 + 4 + 2048];

    // RTP header (12 bytes)
    pkt[0] = 0x80;
    pkt[1] = 0x60 | 96;                         // M=1, PT=96
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

    int sent = sendto(s->udp_audio_rtp_sock, pkt, pkt_len, 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0 || (size_t)sent != pkt_len) {
        apply_udp_backoff(s);
        ESP_LOGW(TAG, "Audio UDP send failed");
    }
}

static void send_rtp_aac(rtsp_session_t *s, const uint8_t *aac, size_t aac_len) {
    if (s->audio_use_tcp) {
        send_rtp_aac_tcp(s, aac, aac_len);
    } else {
        send_rtp_aac_udp(s, aac, aac_len);
    }
    s->audio_seq_num++;
    s->audio_timestamp += AAC_FRAME_SAMPLES;
}

// ---------------------------------------------------------------------------
// Get local IP for SDP
// ---------------------------------------------------------------------------
static void get_local_ip(char *ip_buf, size_t size) {
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        strncpy(ip_buf, "0.0.0.0", size);
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    // Connect doesn't actually send data for UDP
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        strncpy(ip_buf, "0.0.0.0", size);
        return;
    }

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(sock, (struct sockaddr *)&local, &local_len);
    close(sock);

    inet_ntop(AF_INET, &local.sin_addr, ip_buf, size);
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

    // Set accept timeout for periodic checks
    struct timeval accept_tv = { .tv_sec = 0, .tv_usec = 50000 }; // 50ms
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &accept_tv, sizeof(accept_tv));

    while (server_running) {
        // ----- Accept new RTSP control connections -----
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);

        if (client_sock >= 0) {
            char ip_str[16];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Client connected from %s", ip_str);

            // TCP_NODELAY for low latency
            int flag = 1;
            setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            // Send timeout
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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
                    cleanup_session(setup_slot);
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

            // Check for incoming data (TEARDOWN)
            char req_buf2[REQ_BUF_SIZE];
            int n = recv_nonblock(s->ctrl_sock, req_buf2, sizeof(req_buf2));
            if (n > 0) {
                if (strstr(req_buf2, "TEARDOWN")) {
                    int cseq = parse_cseq(req_buf2);
                    handle_teardown(s->ctrl_sock, cseq, i);
                    continue;
                }
                s->last_activity_ms = cur;
            } else if (n < 0) {
                // Connection closed
                ESP_LOGI(TAG, "Client disconnected: session %08lx",
                         (unsigned long)s->session_id);
                cleanup_session(i);
                continue;
            }

            // Session timeout
            if (cur - s->last_activity_ms > SESSION_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Session timeout: %08lx",
                         (unsigned long)s->session_id);
                cleanup_session(i);
                continue;
            }
        }

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
            camera_fb_t *fb = camera_capture();
            if (fb) {
                last_frame_width = fb->width;
                last_frame_height = fb->height;

                for (int i = 0; i < MAX_SESSIONS; i++) {
                    rtsp_session_t *s = sessions[i];
                    if (!s || !s->is_playing) continue;
                    if (cur - s->last_frame_ms < FRAME_INTERVAL_MS) continue;

                    send_rtp_jpeg(s, fb);

                    if (s->last_frame_ms > 0) {
                        uint32_t delta = cur - s->last_frame_ms;
                        uint32_t increment = delta * 90; // 90kHz clock
                        if (increment == 0) increment = 1;
                        s->timestamp += increment;
                    }
                    s->last_frame_ms = cur;
                    s->last_activity_ms = cur;
                }
                camera_return_fb(fb);
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
                if (aac_encoder_pipe_get_frame(aac_buf, sizeof(aac_buf), &aac_len)
                    && aac_len > 0) {
                    for (int i = 0; i < MAX_SESSIONS; i++) {
                        rtsp_session_t *s = sessions[i];
                        if (!s || !s->is_playing || !s->audio_setup) continue;
                        if (cur - s->last_audio_ms < audio_interval) continue;

                        send_rtp_aac(s, aac_buf, aac_len);
                        s->last_audio_ms = cur;
                        s->last_activity_ms = cur;
                    }
                }
            }
        }

        // Yield
        vTaskDelay(1);
    }

    // Cleanup all sessions
    for (int i = 0; i < MAX_SESSIONS; i++) {
        cleanup_session(i);
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

void rtsp_server_set_allow_udp(bool udp_allowed) {
    allow_udp = udp_allowed;
}

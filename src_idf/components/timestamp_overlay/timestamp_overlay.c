#include "timestamp_overlay.h"
#include "font_8x12.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "img_converters.h"

static const char *TAG = "ts_overlay";

#define PADDING_X  6
#define PADDING_Y  4
#define BG_PAD_H   3   // horizontal padding inside background strip
#define BG_PAD_V   2   // vertical padding inside background strip
#define JPEG_QUALITY 80 // re-encode quality (0-100 scale)

static uint8_t *s_rgb_buf = NULL;
static size_t   s_rgb_buf_size = 0;
static uint16_t s_max_width = 0;
static uint16_t s_max_height = 0;
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_enabled = false;
static volatile bool s_cam_name_enabled = false;
static char s_cam_name[25] = "";  // max 24 chars + null

esp_err_t timestamp_overlay_init(uint16_t max_width, uint16_t max_height)
{
    s_max_width = max_width;
    s_max_height = max_height;
    s_rgb_buf_size = (size_t)max_width * max_height * 3;

    s_rgb_buf = heap_caps_malloc(s_rgb_buf_size, MALLOC_CAP_SPIRAM);
    if (!s_rgb_buf) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer (%u bytes)", (unsigned)s_rgb_buf_size);
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        heap_caps_free(s_rgb_buf);
        s_rgb_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized: max %ux%u, RGB buf %u bytes in PSRAM",
             max_width, max_height, (unsigned)s_rgb_buf_size);
    return ESP_OK;
}

bool timestamp_overlay_is_enabled(void)
{
    return s_enabled;
}

void timestamp_overlay_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "Timestamp overlay %s", enabled ? "enabled" : "disabled");
}

bool camera_name_overlay_is_enabled(void)
{
    return s_cam_name_enabled;
}

void camera_name_overlay_set_enabled(bool enabled)
{
    s_cam_name_enabled = enabled;
    ESP_LOGI(TAG, "Camera name overlay %s", enabled ? "enabled" : "disabled");
}

void camera_name_overlay_set_name(const char *name)
{
    if (!name) return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_cam_name, name, sizeof(s_cam_name));
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Camera name set to: %s", name);
}

const char *camera_name_overlay_get_name(void)
{
    return s_cam_name;
}

// Draw a single glyph at (x0, y0) in white on the RGB888 buffer
static void draw_glyph(uint8_t *rgb, uint16_t img_w, uint16_t img_h,
                        int x0, int y0, int glyph_idx)
{
    for (int row = 0; row < FONT_HEIGHT; row++) {
        int y = y0 + row;
        if (y < 0 || y >= img_h) continue;
        uint8_t bits = font_data[glyph_idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            int x = x0 + col;
            if (x < 0 || x >= img_w) continue;
            if (bits & (0x80 >> col)) {
                size_t idx = ((size_t)y * img_w + x) * 3;
                rgb[idx]     = 255; // R
                rgb[idx + 1] = 255; // G
                rgb[idx + 2] = 255; // B
            }
        }
    }
}

// Darken a rectangular region for the background strip
static void darken_region(uint8_t *rgb, uint16_t img_w, uint16_t img_h,
                          int x0, int y0, int w, int h)
{
    for (int row = 0; row < h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= img_h) continue;
        for (int col = 0; col < w; col++) {
            int x = x0 + col;
            if (x < 0 || x >= img_w) continue;
            size_t idx = ((size_t)y * img_w + x) * 3;
            rgb[idx]     /= 3;
            rgb[idx + 1] /= 3;
            rgb[idx + 2] /= 3;
        }
    }
}

bool timestamp_overlay_apply(const uint8_t *jpeg_in, size_t jpeg_len,
                             uint16_t width, uint16_t height,
                             uint8_t quality, overlay_frame_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!s_rgb_buf || !s_mutex) return false;

    // Check resolution fits in pre-allocated buffer
    size_t needed = (size_t)width * height * 3;
    if (needed > s_rgb_buf_size) {
        ESP_LOGW(TAG, "Frame %ux%u exceeds max buffer (%ux%u), skipping overlay",
                 width, height, s_max_width, s_max_height);
        return false;
    }

    // Try to take mutex with short timeout (don't block streaming)
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    // Decode JPEG to RGB888
    if (!fmt2rgb888(jpeg_in, jpeg_len, PIXFORMAT_JPEG, s_rgb_buf)) {
        ESP_LOGW(TAG, "JPEG decode failed");
        xSemaphoreGive(s_mutex);
        return false;
    }

    // Draw timestamp overlay (top-right)
    if (s_enabled) {
        char ts_str[24];
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);

        if (tm.tm_year + 1900 < 2024) {
            snprintf(ts_str, sizeof(ts_str), "NO TIME SYNC");
        } else {
            strftime(ts_str, sizeof(ts_str), "%Y-%m-%d %H:%M:%S", &tm);
        }

        int ts_len = strlen(ts_str);
        int ts_w = ts_len * FONT_WIDTH;

        int bg_x = width - ts_w - 2 * BG_PAD_H - PADDING_X;
        int bg_y = PADDING_Y;
        int bg_w = ts_w + 2 * BG_PAD_H;
        int bg_h = FONT_HEIGHT + 2 * BG_PAD_V;
        if (bg_x < 0) bg_x = 0;

        darken_region(s_rgb_buf, width, height, bg_x, bg_y, bg_w, bg_h);
        int text_x = bg_x + BG_PAD_H;
        int text_y = bg_y + BG_PAD_V;
        for (int i = 0; i < ts_len; i++) {
            draw_glyph(s_rgb_buf, width, height, text_x + i * FONT_WIDTH, text_y,
                        font_char_index(ts_str[i]));
        }
    }

    // Draw camera name overlay (bottom-left)
    if (s_cam_name_enabled && s_cam_name[0] != '\0') {
        int name_len = strlen(s_cam_name);
        int name_w = name_len * FONT_WIDTH;

        int nbg_x = PADDING_X;
        int nbg_y = height - FONT_HEIGHT - 2 * BG_PAD_V - PADDING_Y;
        int nbg_w = name_w + 2 * BG_PAD_H;
        int nbg_h = FONT_HEIGHT + 2 * BG_PAD_V;
        if (nbg_y < 0) nbg_y = 0;

        darken_region(s_rgb_buf, width, height, nbg_x, nbg_y, nbg_w, nbg_h);
        int nx = nbg_x + BG_PAD_H;
        int ny = nbg_y + BG_PAD_V;
        for (int i = 0; i < name_len; i++) {
            draw_glyph(s_rgb_buf, width, height, nx + i * FONT_WIDTH, ny,
                        font_char_index(s_cam_name[i]));
        }
    }

    // Re-encode to JPEG
    uint8_t *jpg_out = NULL;
    size_t jpg_len = 0;
    uint8_t q = quality > 0 ? quality : JPEG_QUALITY;

    if (!fmt2jpg(s_rgb_buf, needed, width, height, PIXFORMAT_RGB888, q, &jpg_out, &jpg_len)) {
        ESP_LOGW(TAG, "JPEG re-encode failed");
        xSemaphoreGive(s_mutex);
        return false;
    }

    xSemaphoreGive(s_mutex);

    out->buf = jpg_out;
    out->len = jpg_len;
    out->width = width;
    out->height = height;
    return true;
}

void timestamp_overlay_release(overlay_frame_t *frame)
{
    if (frame && frame->buf) {
        free(frame->buf);
        frame->buf = NULL;
        frame->len = 0;
    }
}

// Auto-generated master header for embedded web assets
// This file includes all embedded web assets and provides a registry

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "embedded_index.h"
#include "embedded_style.h"
#include "embedded_setup.h"
#include "embedded_wifi-setup.h"
#include "embedded_live.h"
#include "embedded_guide.h"
#include "embedded_ota.h"
#include "embedded_logs.h"
#include "embedded_first-setup.h"
#include "embedded_favicon.h"

// File registry entry
struct EmbeddedFile {
    const char* filename;
    const uint8_t* data;
    size_t size;
    const char* mime_type;
};

// Registry of all embedded files
const struct EmbeddedFile embedded_files[] = {
    {"index.html", index_data, index_size, index_mime},
    {"style.css", style_data, style_size, style_mime},
    {"setup.html", setup_data, setup_size, setup_mime},
    {"wifi-setup.html", wifi_setup_data, wifi_setup_size, wifi_setup_mime},
    {"live.html", live_data, live_size, live_mime},
    {"guide.html", guide_data, guide_size, guide_mime},
    {"ota.html", ota_data, ota_size, ota_mime},
    {"logs.html", logs_data, logs_size, logs_mime},
    {"first-setup.html", first_setup_data, first_setup_size, first_setup_mime},
    {"favicon.ico", favicon_data, favicon_size, favicon_mime}
};

const size_t embedded_files_count = 10;

// Helper function to find file by name
static inline const struct EmbeddedFile* find_embedded_file(const char* filename) {
    for (size_t i = 0; i < embedded_files_count; i++) {
        if (strcmp(filename, embedded_files[i].filename) == 0) {
            return &embedded_files[i];
        }
    }
    return NULL;
}

// src/logger.cpp - COMPLETE FILE
/*
 * Project: HomeKitKnock-S3
 * File: src/logger.cpp
 * Author: Jesse Greene
 */

#include "logger.h"
#include <time.h>

struct LogEntry {
  uint32_t id;
  uint32_t timestampMs;
  LogLevel level;
  String message;
  String timeFormatted;  // mm.dd HH:MM:SS format
};

static const size_t kLogCapacity = 80;
static LogEntry logEntries[kLogCapacity];
static size_t logIndex = 0;
static size_t logCount = 0;
static uint32_t nextLogId = 1;
static bool timeIsSynced = false;

static String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

const char* logLevelToString(LogLevel level) {
  switch (level) {
    case LOG_DEBUG: return "debug";
    case LOG_INFO: return "info";
    case LOG_WARN: return "warn";
    case LOG_ERROR: return "error";
    default: return "info";
  }
}

void initEventLog() {
  logIndex = 0;
  logCount = 0;
  nextLogId = 1;
  timeIsSynced = false;
}

void clearEventLog() {
  // Reset the ring buffer so the UI can start fresh after a clear request.
  initEventLog();
}

void syncTimeFromNTP(const String &timezone) {
  // Timezone strings use POSIX format with automatic DST rules.
  // Examples:
  //   PST8PDT,M3.2.0,M11.1.0 = Pacific Time (auto DST)
  //   EST5EDT,M3.2.0,M11.1.0 = Eastern Time (auto DST)
  //   CET-1CEST,M3.5.0,M10.5.0/3 = Central European Time (auto DST)
  
  const char *tzString = timezone.c_str();
  setenv("TZ", tzString, 1);
  tzset();
  
  // Configure NTP servers.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  // Wait up to 5 seconds for time sync.
  struct tm timeinfo;
  int retries = 10;
  while (!getLocalTime(&timeinfo) && retries-- > 0) {
    delay(500);
  }
  
  if (retries > 0) {
    timeIsSynced = true;
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
    Serial.printf("✅ NTP time synchronized: %s\n", buffer);
  } else {
    Serial.println("⚠️ NTP time sync failed");
  }
}

void logEvent(LogLevel level, const String &message) {
  // Format timestamp as [mm.dd HH:MM:SS] and include milliseconds since boot.
  // Extract emoji if present at start of message.
  String emoji = "";
  String messageText = message;
  
  // Find first space - everything before it might be emoji
  size_t spaceIdx = message.indexOf(' ');
  if (spaceIdx != -1 && spaceIdx <= 4) {
    // Extract potential emoji part
    String potentialEmoji = message.substring(0, spaceIdx);
    // Check if it looks like an emoji (UTF-8 multi-byte sequence)
    if (potentialEmoji.length() > 0 && (uint8_t)potentialEmoji[0] >= 0xC0) {
      emoji = potentialEmoji;
      messageText = message.substring(spaceIdx + 1);
    }
  }
  
  String timeStr = "[millis]";
  if (timeIsSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char buffer[20]; // mm.dd HH:MM:SS
      strftime(buffer, sizeof(buffer), "%m.%d %H:%M:%S", &timeinfo);
      timeStr = "[" + String(buffer) + "]";
    }
  }
  
  uint32_t ms = millis();
  
  // Print to Serial: emoji [time] [ ms] message (using Serial.print to avoid printf issues with emoji)
  if (emoji.length() > 0) {
    Serial.print(emoji);
    Serial.print(" ");
    Serial.print(timeStr);
    Serial.printf(" [ %lums] %s\n", static_cast<unsigned long>(ms), messageText.c_str());
  } else {
    Serial.print(timeStr);
    Serial.printf(" [ %lums] %s\n", static_cast<unsigned long>(ms), message.c_str());
  }

  // Store in ring buffer for web UI.
  LogEntry &entry = logEntries[logIndex];
  entry.id = nextLogId++;
  entry.timestampMs = ms;
  entry.level = level;
  entry.message = messageText.length() > 0 ? messageText : message;
  entry.timeFormatted = emoji + (emoji.length() > 0 ? " " : "") + timeStr + " [ " + String(ms) + "ms]";

  logIndex = (logIndex + 1) % kLogCapacity;
  if (logCount < kLogCapacity) {
    logCount++;
  }
}

String getEventLogJson(uint32_t sinceId) {
  String json = "{\"entries\":[";
  bool first = true;

  size_t start = (logCount < kLogCapacity) ? 0 : logIndex;
  for (size_t i = 0; i < logCount; ++i) {
    size_t idx = (start + i) % kLogCapacity;
    const LogEntry &entry = logEntries[idx];
    if (entry.id <= sinceId) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"id\":";
    json += entry.id;
    json += ",\"ts\":";
    json += entry.timestampMs;
    json += ",\"time\":\"";
    json += entry.timeFormatted;
    json += "\",\"level\":\"";
    json += logLevelToString(entry.level);
    json += "\",\"message\":\"";
    json += jsonEscape(entry.message);
    json += "\"}";
  }
  json += "]}";
  return json;
}

/*
 * Project: HomeKitKnock-S3
 * File: src/logger.cpp
 * Author: Jesse Greene
 */

#include "logger.h"

struct LogEntry {
  uint32_t id;
  uint32_t timestampMs;
  LogLevel level;
  String message;
};

static const size_t kLogCapacity = 80;
static LogEntry logEntries[kLogCapacity];
static size_t logIndex = 0;
static size_t logCount = 0;
static uint32_t nextLogId = 1;

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
}

void logEvent(LogLevel level, const String &message) {
  Serial.println(message);

  LogEntry &entry = logEntries[logIndex];
  entry.id = nextLogId++;
  entry.timestampMs = millis();
  entry.level = level;
  entry.message = message;

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
    json += ",\"level\":\"";
    json += logLevelToString(entry.level);
    json += "\",\"message\":\"";
    json += jsonEscape(entry.message);
    json += "\"}";
  }
  json += "]}";
  return json;
}

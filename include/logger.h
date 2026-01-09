/*
 * Project: HomeKitKnock-S3
 * File: include/logger.h
 * Author: Jesse Greene
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

enum LogLevel {
  LOG_DEBUG = 0,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR
};

// Initialize the in-memory event log buffer.
void initEventLog();

// Record an event in the log and print it to Serial.
void logEvent(LogLevel level, const String &message);

// Return a JSON payload of log entries newer than sinceId.
String getEventLogJson(uint32_t sinceId);

// Clear all in-memory log entries and reset counters.
void clearEventLog();

// Convert a LogLevel to a CSS-friendly string label.
const char* logLevelToString(LogLevel level);

#endif // LOGGER_H

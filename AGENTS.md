<!--
 Project: HomeKitKnock-S3
 File: AGENTS.md
 Author: Jesse Greene
 -->

# AGENTS.md

This file defines how Codex should help with this repo. Keep it short and
actionable so new tasks start with the right context.

## Project Context
- Purpose: ESP32-S3 (Seeed XIAO ESP32-S3 Sense) firmware for a HomeKit doorbell via Scrypted.
- Primary docs: `docs/esp32-s3-doorbell-architecture.md`.
- Build system: PlatformIO (Arduino framework).

## Local Conventions
- Prefer PlatformIO tasks or `pio` CLI for build/upload/monitor.
- Keep `platformio.ini` flags aligned with PSRAM + flash mode for ESP32-S3.
- Document new wiring/pin decisions in the architecture doc.

## What To Do First
- Read `platformio.ini` and `docs/esp32-s3-doorbell-architecture.md` before changes.
- Ask if any GPIO pin mapping or Scrypted endpoint details are undecided.

## Output Expectations
- Prefer verbose comments in code to explain intent and flow.
- Use concise, actionable steps.
- For code changes, cite exact file paths and keep diffs minimal.

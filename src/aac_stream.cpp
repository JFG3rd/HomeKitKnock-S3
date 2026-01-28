/*
 * Project: HomeKitKnock-S3
 * File: src/aac_stream.cpp
 * Author: Jesse Greene
 */

#include "aac_stream.h"
#include "audio.h"
#include "config.h"
#include "logger.h"

extern "C" {
#include "audio_pipeline.h"
#include "raw_stream.h"
#include "aac_encoder.h"
#include "audio_element.h"
}

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

namespace {
constexpr uint16_t kAacFrameSamples = 1024;
constexpr size_t kMaxMicSamples = 2048;  // AUDIO_SAMPLE_RATE is 16k, AAC 8k needs 2x.
constexpr size_t kAacStashSize = 4096;
constexpr size_t kAacFrameMaxBytes = 2048;

SemaphoreHandle_t aacMutex = nullptr;

uint32_t aacSampleRateHz = 16000;
uint32_t aacBitrateBps = 32000;

bool aacReady = false;
audio_pipeline_handle_t aacPipeline = nullptr;
audio_element_handle_t rawWriter = nullptr;
audio_element_handle_t aacEncoder = nullptr;
audio_element_handle_t rawReader = nullptr;

uint8_t aacStash[kAacStashSize];
size_t aacStashLen = 0;

int16_t micBuffer[kMaxMicSamples];
int16_t pcmFrame[kAacFrameSamples];

struct AdtsInfo {
  size_t frameLen;
  size_t headerLen;
};

uint8_t freqIndexFromRate(uint32_t rateHz) {
  switch (rateHz) {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    case 12000: return 9;
    case 11025: return 10;
    case 8000: return 11;
    case 7350: return 12;
    default: return 8; // 16 kHz fallback.
  }
}

bool parseAdtsHeader(const uint8_t *data, size_t len, AdtsInfo &info) {
  if (!data || len < 7) {
    return false;
  }
  if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
    return false;
  }
  bool protectionAbsent = data[1] & 0x01;
  size_t frameLen = ((data[3] & 0x03) << 11) | (static_cast<size_t>(data[4]) << 3) | ((data[5] & 0xE0) >> 5);
  size_t headerLen = protectionAbsent ? 7 : 9;
  if (frameLen < headerLen) {
    return false;
  }
  info.frameLen = frameLen;
  info.headerLen = headerLen;
  return true;
}

void buildAdtsHeader(uint8_t *out, size_t payloadLen, uint32_t sampleRateHz, uint8_t channels) {
  if (!out) {
    return;
  }
  uint8_t freqIdx = freqIndexFromRate(sampleRateHz);
  uint8_t profile = 2;  // AAC-LC.
  uint16_t frameLen = static_cast<uint16_t>(payloadLen + 7);

  out[0] = 0xFF;
  out[1] = 0xF1;  // 1111 0001 (sync + MPEG-4 + no CRC)
  out[2] = static_cast<uint8_t>(((profile - 1) << 6) | (freqIdx << 2) | ((channels & 0x4) >> 2));
  out[3] = static_cast<uint8_t>(((channels & 0x3) << 6) | ((frameLen >> 11) & 0x03));
  out[4] = static_cast<uint8_t>((frameLen >> 3) & 0xFF);
  out[5] = static_cast<uint8_t>(((frameLen & 0x7) << 5) | 0x1F);
  out[6] = 0xFC;
}

void ensureMutex() {
  if (!aacMutex) {
    aacMutex = xSemaphoreCreateMutex();
  }
}

bool lockAac(uint32_t timeoutMs) {
  ensureMutex();
  if (!aacMutex) {
    return true;
  }
  return xSemaphoreTake(aacMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockAac() {
  if (aacMutex) {
    xSemaphoreGive(aacMutex);
  }
}

void downsample(const int16_t *in, size_t inSamples, int16_t *out, size_t outSamples, uint32_t inRate, uint32_t outRate) {
  if (!in || !out || outSamples == 0) {
    return;
  }
  if (inRate == outRate) {
    size_t copySamples = min(inSamples, outSamples);
    memcpy(out, in, copySamples * sizeof(int16_t));
    if (copySamples < outSamples) {
      memset(out + copySamples, 0, (outSamples - copySamples) * sizeof(int16_t));
    }
    return;
  }
  size_t step = inRate / outRate;
  if (step < 1) {
    step = 1;
  }
  for (size_t i = 0; i < outSamples; i++) {
    size_t idx = i * step;
    out[i] = idx < inSamples ? in[idx] : 0;
  }
}

bool initAacPipeline(uint32_t sampleRateHz, uint32_t bitrateBps) {
  if (aacReady) {
    return true;
  }

  audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
  aacPipeline = audio_pipeline_init(&pipeline_cfg);
  if (!aacPipeline) {
    logEvent(LOG_ERROR, "❌ AAC pipeline init failed");
    return false;
  }

  raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
  raw_cfg.type = AUDIO_STREAM_WRITER;
  raw_cfg.out_rb_size = 4 * 1024;
  rawWriter = raw_stream_init(&raw_cfg);
  if (!rawWriter) {
    logEvent(LOG_ERROR, "❌ AAC raw writer init failed");
    return false;
  }

  raw_cfg.type = AUDIO_STREAM_READER;
  raw_cfg.out_rb_size = 4 * 1024;
  rawReader = raw_stream_init(&raw_cfg);
  if (!rawReader) {
    logEvent(LOG_ERROR, "❌ AAC raw reader init failed");
    return false;
  }

  aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
  aac_cfg.sample_rate = static_cast<int>(sampleRateHz);
  aac_cfg.channel = 1;
  aac_cfg.bitrate = static_cast<int>(bitrateBps);
  aac_cfg.task_core = STREAM_TASK_CORE;
  aac_cfg.out_rb_size = 4 * 1024;
  aacEncoder = aac_encoder_init(&aac_cfg);
  if (!aacEncoder) {
    logEvent(LOG_ERROR, "❌ AAC encoder init failed");
    return false;
  }

  audio_pipeline_register(aacPipeline, rawWriter, "raw_in");
  audio_pipeline_register(aacPipeline, aacEncoder, "aac");
  audio_pipeline_register(aacPipeline, rawReader, "raw_out");

  const char *link_tag[3] = {"raw_in", "aac", "raw_out"};
  audio_pipeline_link(aacPipeline, link_tag, 3);

  audio_element_set_input_timeout(rawWriter, pdMS_TO_TICKS(50));
  audio_element_set_output_timeout(rawReader, pdMS_TO_TICKS(50));

  if (audio_pipeline_run(aacPipeline) != ESP_OK) {
    logEvent(LOG_ERROR, "❌ AAC pipeline start failed");
    return false;
  }

  aacStashLen = 0;
  aacReady = true;
  return true;
}

void deinitAacPipeline() {
  if (!aacReady) {
    return;
  }
  if (aacPipeline) {
    audio_pipeline_stop(aacPipeline);
    audio_pipeline_wait_for_stop(aacPipeline);
    audio_pipeline_terminate(aacPipeline);
  }
  if (aacPipeline && rawWriter) {
    audio_pipeline_unregister(aacPipeline, rawWriter);
  }
  if (aacPipeline && aacEncoder) {
    audio_pipeline_unregister(aacPipeline, aacEncoder);
  }
  if (aacPipeline && rawReader) {
    audio_pipeline_unregister(aacPipeline, rawReader);
  }
  if (rawWriter) {
    audio_element_deinit(rawWriter);
    rawWriter = nullptr;
  }
  if (aacEncoder) {
    audio_element_deinit(aacEncoder);
    aacEncoder = nullptr;
  }
  if (rawReader) {
    audio_element_deinit(rawReader);
    rawReader = nullptr;
  }
  if (aacPipeline) {
    audio_pipeline_deinit(aacPipeline);
    aacPipeline = nullptr;
  }
  aacReady = false;
  aacStashLen = 0;
}

bool readEncodedFrame(uint8_t *out, size_t outMax, size_t &outLen) {
  outLen = 0;
  if (!rawReader || !out || outMax == 0) {
    return false;
  }

  uint32_t startMs = millis();
  uint8_t temp[512];

  while (millis() - startMs < 80) {
    int read = raw_stream_read(rawReader, reinterpret_cast<char *>(temp), sizeof(temp));
    if (read > 0) {
      size_t copy = min(static_cast<size_t>(read), kAacStashSize - aacStashLen);
      memcpy(aacStash + aacStashLen, temp, copy);
      aacStashLen += copy;
    }

    if (aacStashLen < 7) {
      vTaskDelay(1);
      continue;
    }

    AdtsInfo info;
    if (parseAdtsHeader(aacStash, aacStashLen, info)) {
      if (aacStashLen < info.frameLen) {
        vTaskDelay(1);
        continue;
      }
      size_t rawLen = info.frameLen - info.headerLen;
      if (rawLen > outMax) {
        rawLen = outMax;
      }
      memcpy(out, aacStash + info.headerLen, rawLen);
      outLen = rawLen;

      size_t remaining = aacStashLen - info.frameLen;
      if (remaining > 0) {
        memmove(aacStash, aacStash + info.frameLen, remaining);
      }
      aacStashLen = remaining;
      return outLen > 0;
    }

    if (aacStashLen > 0) {
      size_t rawLen = min(aacStashLen, outMax);
      memcpy(out, aacStash, rawLen);
      outLen = rawLen;
      aacStashLen = 0;
      return outLen > 0;
    }

    vTaskDelay(1);
  }

  return false;
}

bool encodeAacFromMic(uint8_t *out, size_t outMax, size_t &outLen) {
  outLen = 0;
  if (!out || outMax == 0) {
    return false;
  }
  if (!lockAac(200)) {
    return false;
  }

  uint32_t sampleRateHz = aacSampleRateHz;
  uint32_t bitrateBps = aacBitrateBps;

  if (!aacReady || !aacPipeline || !rawWriter || !rawReader || !aacEncoder) {
    deinitAacPipeline();
    if (!initAacPipeline(sampleRateHz, bitrateBps)) {
      unlockAac();
      return false;
    }
  }

  size_t inputSamples = (AUDIO_SAMPLE_RATE / sampleRateHz) * kAacFrameSamples;
  if (inputSamples > kMaxMicSamples) {
    inputSamples = kMaxMicSamples;
  }

  bool ok = !isMicMuted() && captureMicSamples(micBuffer, inputSamples, 80);
  if (!ok) {
    memset(micBuffer, 0, inputSamples * sizeof(int16_t));
  }

  downsample(micBuffer, inputSamples, pcmFrame, kAacFrameSamples, AUDIO_SAMPLE_RATE, sampleRateHz);

  int bytesWritten = raw_stream_write(rawWriter,
                                      reinterpret_cast<char *>(pcmFrame),
                                      kAacFrameSamples * sizeof(int16_t));
  if (bytesWritten <= 0) {
    unlockAac();
    return false;
  }

  bool frameOk = readEncodedFrame(out, outMax, outLen);
  unlockAac();
  return frameOk;
}
} // namespace

void setAacStreamConfig(uint32_t sampleRateHz, uint32_t bitrateBps) {
  if (sampleRateHz == 0) {
    sampleRateHz = 16000;
  }
  if (bitrateBps == 0) {
    bitrateBps = 32000;
  }
  if (sampleRateHz != aacSampleRateHz || bitrateBps != aacBitrateBps) {
    if (lockAac(200)) {
      aacSampleRateHz = sampleRateHz;
      aacBitrateBps = bitrateBps;
      deinitAacPipeline();
      unlockAac();
    }
  }
}

AacStreamConfig getAacStreamConfig() {
  return {aacSampleRateHz, aacBitrateBps};
}

uint16_t getAacFrameSamples() {
  return kAacFrameSamples;
}

String getAacSdpRtpmap() {
  return String("MPEG4-GENERIC/") + String(aacSampleRateHz) + "/1";
}

String getAacSdpFmtp() {
  uint8_t freqIdx = freqIndexFromRate(aacSampleRateHz);
  uint16_t asc = static_cast<uint16_t>((2 << 11) | (freqIdx << 7) | (1 << 3));
  char hex[5];
  snprintf(hex, sizeof(hex), "%04X", asc);
  String configHex(hex);
  String fmtp = "profile-level-id=1;mode=AAC-hbr;config=" + configHex;
  fmtp += ";SizeLength=13;IndexLength=3;IndexDeltaLength=3";
  return fmtp;
}

bool getAacRawFrameFromMic(uint8_t *out, size_t outMax, size_t &outLen) {
  return encodeAacFromMic(out, outMax, outLen);
}

bool getAacAdtsFrameFromMic(uint8_t *out, size_t outMax, size_t &outLen) {
  outLen = 0;
  if (!out || outMax < 8) {
    return false;
  }

  uint8_t raw[kAacFrameMaxBytes];
  size_t rawLen = 0;
  if (!encodeAacFromMic(raw, sizeof(raw), rawLen)) {
    return false;
  }

  size_t totalLen = rawLen + 7;
  if (totalLen > outMax) {
    return false;
  }
  buildAdtsHeader(out, rawLen, aacSampleRateHz, 1);
  memcpy(out + 7, raw, rawLen);
  outLen = totalLen;
  return true;
}

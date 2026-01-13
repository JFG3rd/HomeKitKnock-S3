/*
 * Project: HomeKitKnock-S3
 * File: src/audio.cpp
 * Author: Jesse Greene
 */

#include "audio.h"
#include "config.h"
#include "logger.h"

#include <LittleFS.h>
#include <driver/i2s.h>
#include <math.h>
#include <limits.h>
#include <string.h>

static const char *kGongPcmPath = "/gong.pcm";
static const uint32_t kAudioSampleRate = AUDIO_SAMPLE_RATE;
static const uint16_t kAudioBitsPerSample = AUDIO_SAMPLE_BITS;

static bool micEnabled = false;
static bool micMuted = false;
static uint8_t micSensitivity = DEFAULT_MIC_SENSITIVITY;
static bool micI2sReady = false;

static bool audioOutEnabled = false;
static bool audioOutMuted = false;
static uint8_t audioOutVolume = DEFAULT_AUDIO_OUT_VOLUME;
static bool audioOutI2sReady = false;

static bool gongTaskRunning = false;

static int16_t scaleSample(int16_t sample, uint8_t percent) {
  if (percent >= 100) {
    return sample;
  }
  int32_t scaled = static_cast<int32_t>(sample) * percent / 100;
  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }
  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(scaled);
}

static bool initMicI2s() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = kAudioSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 512;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  if (err != ESP_OK) {
    logEvent(LOG_ERROR, "Mic I2S install failed");
    return false;
  }

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = I2S_PDM_MIC_CLK;
  pin_config.ws_io_num = I2S_PIN_NO_CHANGE;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = I2S_PDM_MIC_DATA;
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    logEvent(LOG_ERROR, "Mic I2S set pin failed");
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  i2s_set_clk(I2S_NUM_0, kAudioSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  micI2sReady = true;
  return true;
}

static void deinitMicI2s() {
  if (micI2sReady) {
    i2s_driver_uninstall(I2S_NUM_0);
    micI2sReady = false;
  }
}

static bool initAudioOutI2s() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kAudioSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_NUM_1, &config, 0, NULL);
  if (err != ESP_OK) {
    logEvent(LOG_ERROR, "Audio I2S install failed");
    return false;
  }

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num = I2S_DAC_BCLK;
  pin_config.ws_io_num = I2S_DAC_LRCLK;
  pin_config.data_out_num = I2S_DAC_DOUT;
  pin_config.data_in_num = I2S_PIN_NO_CHANGE;
  err = i2s_set_pin(I2S_NUM_1, &pin_config);
  if (err != ESP_OK) {
    logEvent(LOG_ERROR, "Audio I2S set pin failed");
    i2s_driver_uninstall(I2S_NUM_1);
    return false;
  }

  i2s_set_clk(I2S_NUM_1, kAudioSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_NUM_1);
  audioOutI2sReady = true;
  return true;
}

static void deinitAudioOutI2s() {
  if (audioOutI2sReady) {
    i2s_driver_uninstall(I2S_NUM_1);
    audioOutI2sReady = false;
  }
}

void configureAudio(bool micEnable,
                    bool micMute,
                    uint8_t micSensitivityValue,
                    bool audioOutEnableValue,
                    bool audioOutMuteValue,
                    uint8_t audioOutVolumeValue) {
  micEnabled = micEnable;
  micMuted = micMute;
  micSensitivity = micSensitivityValue;
  audioOutEnabled = audioOutEnableValue;
  audioOutMuted = audioOutMuteValue;
  audioOutVolume = audioOutVolumeValue;

  if (micEnabled && !micI2sReady) {
    initMicI2s();
  } else if (!micEnabled && micI2sReady) {
    deinitMicI2s();
  }

  if (audioOutEnabled && !audioOutI2sReady) {
    initAudioOutI2s();
  } else if (!audioOutEnabled && audioOutI2sReady) {
    deinitAudioOutI2s();
  }
}

bool isMicEnabled() {
  return micEnabled;
}

bool isMicMuted() {
  return micMuted;
}

uint8_t getMicSensitivity() {
  return micSensitivity;
}

bool isAudioOutEnabled() {
  return audioOutEnabled;
}

bool isAudioOutMuted() {
  return audioOutMuted;
}

uint8_t getAudioOutVolume() {
  return audioOutVolume;
}

bool captureMicSamples(int16_t *buffer, size_t sampleCount, uint32_t timeoutMs) {
  if (!buffer || sampleCount == 0) {
    return false;
  }
  if (!micEnabled) {
    return false;
  }
  if (micMuted) {
    memset(buffer, 0, sampleCount * sizeof(int16_t));
    return true;
  }
  if (!micI2sReady && !initMicI2s()) {
    return false;
  }

  size_t bytesNeeded = sampleCount * sizeof(int16_t);
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, buffer, bytesNeeded, &bytesRead, pdMS_TO_TICKS(timeoutMs));
  if (err != ESP_OK || bytesRead == 0) {
    memset(buffer, 0, bytesNeeded);
    return false;
  }

  size_t samplesRead = bytesRead / sizeof(int16_t);
  for (size_t i = 0; i < samplesRead; i++) {
    buffer[i] = scaleSample(buffer[i], micSensitivity);
  }
  if (samplesRead < sampleCount) {
    memset(buffer + samplesRead, 0, (sampleCount - samplesRead) * sizeof(int16_t));
  }
  return true;
}

static void writeSamples(const int16_t *samples, size_t sampleCount) {
  if (!samples || sampleCount == 0 || !audioOutI2sReady) {
    return;
  }
  size_t bytesToWrite = sampleCount * sizeof(int16_t);
  size_t bytesWritten = 0;
  i2s_write(I2S_NUM_1, samples, bytesToWrite, &bytesWritten, portMAX_DELAY);
}

static void gongTask(void *pvParameters) {
  gongTaskRunning = true;

  if (!audioOutEnabled || audioOutMuted) {
    gongTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }
  if (!audioOutI2sReady && !initAudioOutI2s()) {
    gongTaskRunning = false;
    vTaskDelete(NULL);
    return;
  }

  if (LittleFS.exists(kGongPcmPath)) {
    File file = LittleFS.open(kGongPcmPath, "r");
    if (file) {
      int16_t buffer[256];
      while (file.available()) {
        size_t bytes = file.read(reinterpret_cast<uint8_t *>(buffer), sizeof(buffer));
        size_t samples = bytes / sizeof(int16_t);
        if (samples == 0) {
          break;
        }
        for (size_t i = 0; i < samples; i++) {
          buffer[i] = scaleSample(buffer[i], audioOutVolume);
        }
        writeSamples(buffer, samples);
      }
      file.close();
    }
  } else {
    const float toneA = 880.0f;
    const float toneB = 660.0f;
    const uint32_t samplesPerTone = kAudioSampleRate / 3;
    float phase = 0.0f;
    float phaseStep = 2.0f * static_cast<float>(M_PI) * toneA / kAudioSampleRate;
    int16_t buffer[256];
    for (uint32_t i = 0; i < samplesPerTone; i += 256) {
      size_t chunk = min(samplesPerTone - i, static_cast<uint32_t>(256));
      float envelope = 1.0f - static_cast<float>(i) / samplesPerTone;
      for (size_t j = 0; j < chunk; j++) {
        float sample = sinf(phase) * envelope;
        buffer[j] = scaleSample(static_cast<int16_t>(sample * 16000.0f), audioOutVolume);
        phase += phaseStep;
        if (phase > 2.0f * static_cast<float>(M_PI)) {
          phase -= 2.0f * static_cast<float>(M_PI);
        }
      }
      writeSamples(buffer, chunk);
    }

    phase = 0.0f;
    phaseStep = 2.0f * static_cast<float>(M_PI) * toneB / kAudioSampleRate;
    for (uint32_t i = 0; i < samplesPerTone; i += 256) {
      size_t chunk = min(samplesPerTone - i, static_cast<uint32_t>(256));
      float envelope = 1.0f - static_cast<float>(i) / samplesPerTone;
      for (size_t j = 0; j < chunk; j++) {
        float sample = sinf(phase) * envelope;
        buffer[j] = scaleSample(static_cast<int16_t>(sample * 14000.0f), audioOutVolume);
        phase += phaseStep;
        if (phase > 2.0f * static_cast<float>(M_PI)) {
          phase -= 2.0f * static_cast<float>(M_PI);
        }
      }
      writeSamples(buffer, chunk);
    }
  }

  gongTaskRunning = false;
  vTaskDelete(NULL);
}

void playGongAsync() {
  if (!audioOutEnabled || audioOutMuted) {
    return;
  }
  if (gongTaskRunning) {
    return;
  }
  xTaskCreatePinnedToCore(
      gongTask,
      "gong_task",
      4096,
      NULL,
      1,
      NULL,
      STREAM_TASK_CORE);
}

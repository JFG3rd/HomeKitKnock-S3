/*
 * Project: HomeKitKnock-S3
 * File: src/cameraAPI.cpp
 * Author: Jesse Greene
 */

#include "cameraAPI.h"
#ifdef CAMERA

#include <Arduino.h>
#include <Preferences.h>
#include <esp_camera.h>

static const char *kCameraPrefsNamespace = "camera";
static const char *kCameraFramesizeKey = "framesize";
static const char *kCameraQualityKey = "quality";
static const char *kCameraBrightnessKey = "brightness";
static const char *kCameraContrastKey = "contrast";
static const int kCameraUnsetValue = -99;

// Apply a single sensor parameter update based on URL args.
static void setControl(String variable, int value) {
  sensor_t *s = esp_camera_sensor_get();
  if (variable.startsWith("framesize")) {
    s->set_framesize(s, (framesize_t)value);
  } else if (variable.startsWith("quality")) {
    s->set_quality(s, value);
  } else if (variable.startsWith("contrast")) {
    s->set_contrast(s, value);
  } else if (variable.startsWith("brightness")) {
    s->set_brightness(s, value);
  } else if (variable.startsWith("saturation")) {
    s->set_saturation(s, value);
  } else if (variable.startsWith("gainceiling")) {
    s->set_gainceiling(s, (gainceiling_t)value);
  } else if (variable.startsWith("colorbar")) {
    s->set_colorbar(s, value);
  } else if (variable.startsWith("awb")) {
    s->set_whitebal(s, value);
  } else if (variable.startsWith("agc")) {
    s->set_gain_ctrl(s, value);
  } else if (variable.startsWith("aec")) {
    s->set_exposure_ctrl(s, value);
  } else if (variable.startsWith("hmirror")) {
    s->set_hmirror(s, value);
  } else if (variable.startsWith("vflip")) {
    s->set_vflip(s, value);
  } else if (variable.startsWith("awb_gain")) {
    s->set_awb_gain(s, value);
  } else if (variable.startsWith("agc_gain")) {
    s->set_agc_gain(s, value);
  } else if (variable.startsWith("aec_value")) {
    s->set_aec_value(s, value);
  } else if (variable.startsWith("aec2")) {
    s->set_aec2(s, value);
  } else if (variable.startsWith("dcw")) {
    s->set_dcw(s, value);
  } else if (variable.startsWith("bpc")) {
    s->set_bpc(s, value);
  } else if (variable.startsWith("wpc")) {
    s->set_wpc(s, value);
  } else if (variable.startsWith("raw_gma")) {
    s->set_raw_gma(s, value);
  } else if (variable.startsWith("lenc")) {
    s->set_lenc(s, value);
  } else if (variable.startsWith("special_effect")) {
    s->set_special_effect(s, value);
  } else if (variable.startsWith("wb_mode")) {
    s->set_wb_mode(s, value);
  } else if (variable.startsWith("ae_level")) {
    s->set_ae_level(s, value);
  }
}

// Return a JSON snapshot of sensor settings for debugging/UX.
static void handleStatus(AsyncWebServerRequest *request) {
  static char json_response[1024];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';
  if (s->id.PID == NT99141_PID) {
    p += sprintf(p, "\"PID\":\"NT99141_PID\",");
  } else if (s->id.PID == OV9650_PID) {
    p += sprintf(p, "\"PID\":\"OV9650_PID\",");
  } else if (s->id.PID == OV7725_PID) {
    p += sprintf(p, "\"PID\":\"OV7725_PID\",");
  } else if (s->id.PID == OV2640_PID) {
    p += sprintf(p, "\"PID\":\"OV2640_PID\",");
  } else if (s->id.PID == OV3660_PID) {
    p += sprintf(p, "\"PID\":\"OV3660_PID\",");
  } else if (s->id.PID == OV5640_PID) {
    p += sprintf(p, "\"PID\":\"OV5640_PID\",");
  } else if (s->id.PID == OV7670_PID) {
    p += sprintf(p, "\"PID\":\"OV7670_PID\",");
  }
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p += sprintf(p, "\"awb\":%u,", s->status.awb);
  p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p += sprintf(p, "\"aec\":%u,", s->status.aec);
  p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p += sprintf(p, "\"agc\":%u,", s->status.agc);
  p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
  *p++ = '}';
  *p++ = 0;
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json_response);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

// Capture a single JPEG frame and return it inline.
static void handleSnap(AsyncWebServerRequest *request) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    log_e("Camera capture failed");
    request->send(500);
    return;
  }

  AsyncResponseStream *response = request->beginResponseStream("image/jpeg");
  response->addHeader("Content-Disposition", "inline; filename=capture.jpg");
  response->write(fb->buf, fb->len);
  request->send(response);

  esp_camera_fb_return(fb);
}

// Apply control changes via /control?var=...&val=...
static void handleControl(AsyncWebServerRequest *request) {
  String variable = request->arg("var");
  String value = request->arg("val");
  int intValue = atoi(value.c_str());

  setControl(variable, intValue);
  if (variable.startsWith("framesize") || variable.startsWith("quality") ||
      variable.startsWith("brightness") || variable.startsWith("contrast")) {
    // Persist key camera settings so they survive reboots.
    Preferences prefs;
    if (prefs.begin(kCameraPrefsNamespace, false)) {
      if (variable.startsWith("framesize")) {
        prefs.putInt(kCameraFramesizeKey, intValue);
      } else if (variable.startsWith("quality")) {
        prefs.putInt(kCameraQualityKey, intValue);
      } else if (variable.startsWith("brightness")) {
        prefs.putInt(kCameraBrightnessKey, intValue);
      } else if (variable.startsWith("contrast")) {
        prefs.putInt(kCameraContrastKey, intValue);
      }
      prefs.end();
    }
  }
  request->send(200, "text/plain", "Set " + variable + " to " + value);
}

// Initialize the camera with board-specific pins and baseline tuning.
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  // Lower XCLK for stability on some OV2640 modules.
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  // Prefer PSRAM for higher resolution and double-buffering.
  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  // Apply sensor-specific defaults for common modules.
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 0);
    s->set_saturation(s, 3);
    s->set_sharpness(s, 3);
  } else {
    s->set_saturation(s, 2);
    s->set_aec2(s, true);
    s->set_gainceiling(s, GAINCEILING_128X);
    s->set_lenc(s, true);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif
#if defined(CAMERA_MODEL_JSZWY_CYIS) || defined(CAMERA_MODEL_JSZWY_CYIS_2)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif
  int defaultFramesize = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  int defaultQuality = psramFound() ? 10 : 12;
  int storedFramesize = -1;
  int storedQuality = -1;
  int storedBrightness = kCameraUnsetValue;
  int storedContrast = kCameraUnsetValue;
  Preferences prefs;
  if (prefs.begin(kCameraPrefsNamespace, true)) {
    storedFramesize = prefs.getInt(kCameraFramesizeKey, -1);
    storedQuality = prefs.getInt(kCameraQualityKey, -1);
    storedBrightness = prefs.getInt(kCameraBrightnessKey, kCameraUnsetValue);
    storedContrast = prefs.getInt(kCameraContrastKey, kCameraUnsetValue);
    prefs.end();
  }
  s->set_framesize(s, (framesize_t)(storedFramesize >= 0 ? storedFramesize : defaultFramesize));
  s->set_quality(s, storedQuality >= 0 ? storedQuality : defaultQuality);
  if (storedBrightness != kCameraUnsetValue) {
    s->set_brightness(s, storedBrightness);
  }
  if (storedContrast != kCameraUnsetValue) {
    s->set_contrast(s, storedContrast);
  }

  return true;
}

// Register AsyncWebServer endpoints and a redirect to port 81 MJPEG stream.
void registerCameraEndpoints(AsyncWebServer &server) {
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleSnap);
  server.on("/control", HTTP_GET, handleControl);
  server.on("/stream", HTTP_GET, [](AsyncWebServerRequest *request) {
    String target = "http://" + request->host() + ":81/stream";
    request->redirect(target);
  });
}

#endif // CAMERA

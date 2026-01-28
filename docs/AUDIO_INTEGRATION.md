<!--
 Project: HomeKitKnock-S3
 File: AUDIO_INTEGRATION.md
 Author: Jesse Greene
 -->

# Audio Integration Plan

This document outlines the plan to integrate the Seeed XIAO ESP32-S3 Sense onboard microphone into the HTTP and RTSP video streams.

## Current Implementation (v1.3.3+)

- **RTSP audio**: AAC-LC (MPEG4-GENERIC) is advertised only when the mic feature is enabled.
- **HTTP audio preview**: `/audio.wav` returns a short WAV clip for browser testing.
- **Continuous HTTP audio**: `http://ESP32-IP:81/audio.aac` streams AAC (ADTS) as a companion to MJPEG.
- **Browser A/V page**: `http://ESP32-IP/live` pairs MJPEG + AAC (click-to-play audio).
- **Audio out**: MAX98357A I2S DAC plays gong/tone clips when enabled.
- **SIP intercom audio**: RTP G.711 (PCMU/PCMA) with RFC2833 DTMF support for door opener relay.
- **HTTP client limit**: MJPEG and AAC streams share the same max client cap.
- **Mic sharing**: RTSP and HTTP audio share the same mic capture path; avoid running both if audio stutters.
- **HTTP dependency**: AAC streaming is available only when HTTP camera streaming is enabled.
- **Compatibility**:
  - **Scrypted/HomeKit**: prefer RTSP for A/V (Scrypted can transcode).
  - **FRITZ!Box**: SIP carries G.711 RTP audio + DTMF; HTTP audio is not used by the intercom.

## Hardware

**Microphone**: MSM261S4030H0R (Pulse Density Modulation - PDM)
- **Interface**: I2S PDM mode
- **Sample Rate**: 16 kHz typical (supports up to 48 kHz)
- **Bit Depth**: 16-bit PCM after conversion
- **Pins** (ESP32-S3):
  - **CLK**: GPIO42 (I2S_SCK)
  - **DATA**: GPIO41 (I2S_SD)

**Speaker DAC**: MAX98357A (I2S DAC + Class-D amp)
- **Interface**: I2S (standard)
- **Power**: 3V3 + GND
- **Pins** (ESP32-S3, free header pins):
  - **BCLK**: GPIO7 (D8 / A8 / SCK)
  - **LRCLK/WS**: GPIO8 (D9 / A9 / MISO)
  - **DIN**: GPIO9 (D10 / A10 / MOSI)
  - **SC**: tie to 3V3 (always on)

**Notes**:
- GPIO7/8/9 are free on the XIAO ESP32-S3 Sense and do not overlap the camera pin map in `include/camera_pins.h`.
- GPIO7/8/9 are also default SPI pins; avoid SPI usage or remap if you need both.
- Use separate I2S peripherals: `I2S_NUM_0` for PDM mic RX, `I2S_NUM_1` for DAC TX.

## Implementation Requirements

### 1. I2S PDM Configuration

Configure ESP32-S3 I2S peripheral for PDM microphone input:

```cpp
#include <driver/i2s.h>

#define I2S_PDM_MIC_CLK   42
#define I2S_PDM_MIC_DATA  41
#define I2S_SAMPLE_RATE   16000
#define I2S_BUFFER_SIZE   512

i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = I2S_BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
};

i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_PDM_MIC_CLK,
    .ws_io_num = I2S_PIN_NO_CHANGE,  // Not used in PDM mode
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_PDM_MIC_DATA
};

i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_config);
i2s_set_clk(I2S_NUM_0, I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
```

### 2. Audio Encoding Options

#### Option A: AAC-LC (Selected for RTSP + HTTP)
- **Codec**: AAC-LC (MPEG4-GENERIC)
- **RTP Payload Type**: 96 (dynamic)
- **Bitrate**: 16–32 kbps @ 8 kHz, or 24–48 kbps @ 16 kHz (configurable in `/setup`)
- **Pros**: Good quality at low bitrate, supported by Scrypted/HomeKit via transcoding
- **Cons**: More CPU than G.711, needs ESP-ADF libs

#### Option B: G.711 (Still used for SIP intercom)
- **Codec**: G.711 μ-law / A-law (PCMU/PCMA)
- **Bitrate**: 64 kbps (8 kHz, mono)
- **Pros**: Universal SIP compatibility, low CPU
- **Cons**: Lower quality than AAC

### 2a. HTML/Browser Audio Support

Browsers do not play raw G.711. For HTML views:
- **Preferred**: `audio/aac` (ADTS) via `/audio.aac` for streaming
- **Preview**: `audio/wav` (PCM 16-bit, mono, 8-16 kHz) via `/audio.wav`
- **Alternate**: WebRTC + Opus for full A/V sync (larger scope)
- **Avoid**: raw PCMU/G.711 in HTML `<audio>` tags (not supported)

### 3. RTSP Integration

#### SDP Modification

Add audio stream to RTSP DESCRIBE response:

```cpp
String sdp = "v=0\r\n";
sdp += "o=- 0 0 IN IP4 " + WiFi.localIP().toString() + "\r\n";
sdp += "s=ESP32-S3 Camera\r\n";
sdp += "c=IN IP4 " + WiFi.localIP().toString() + "\r\n";
sdp += "t=0 0\r\n";

// Video stream (MJPEG)
sdp += "m=video 0 RTP/AVP 26\r\n";
sdp += "a=rtpmap:26 JPEG/90000\r\n";
sdp += "a=control:track1\r\n";

// Audio stream (AAC-LC)
sdp += "m=audio 0 RTP/AVP 96\r\n";
sdp += "a=rtpmap:96 MPEG4-GENERIC/16000/1\r\n";
sdp += "a=fmtp:96 profile-level-id=1;mode=AAC-hbr;config=1408;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n";
sdp += "a=control:track2\r\n";
```

Implementation notes:
- Audio is advertised only when mic is enabled in Feature Setup.
- Track mapping: `track1` = video, `track2` = audio (AAC-LC).

#### RTP Audio Packet Format (AAC-LC / MPEG4-GENERIC)

AAC-LC RTP payload uses AU headers (RFC 3640):
```
[RTP Header 12 bytes] + [AU-headers-length 2 bytes] + [AU-header 2 bytes] + [AAC raw frame]
```

RTP header for audio:
- **Version**: 2
- **Payload Type**: 96 (dynamic)
- **Sequence Number**: Incremented per packet
- **Timestamp**: Sample count (clock = sample rate; +1024 per AAC frame)
- **SSRC**: Unique audio stream identifier

AU header fields:
- **AU-headers-length**: 16 (bits)
- **AU-header**: `(frame_size_bytes << 3)`

Sample packing:
```cpp
const uint16_t auHeaderLenBits = 16;
const uint16_t auHeader = (frameLen & 0x1FFF) << 3;

payload[0] = auHeaderLenBits >> 8;
payload[1] = auHeaderLenBits & 0xFF;
payload[2] = auHeader >> 8;
payload[3] = auHeader & 0xFF;
memcpy(payload + 4, frame, frameLen);
```

### 4. SIP Audio (G.711)

SIP intercom uses its own RTP path (PCMU/PCMA + RFC2833 DTMF).
See `docs/SIP_INTEGRATION.md` for the SIP RTP details and door opener handling.

### 5. HTTP Stream Integration

HTTP MJPEG streams don't natively support audio. Options:

#### Option A: Separate Audio Stream (Implemented)
- Continuous AAC (ADTS) stream at `http://ESP32-IP:81/audio.aac`
- Short WAV preview at `http://ESP32-IP/audio.wav`
- MJPEG remains at `http://ESP32-IP:81/stream` (pair manually or use `/live`)
- Clients must sync audio/video manually

#### Option B: WebRTC/HLS (Planned)
- Use WebRTC or HLS for combined A/V streaming
- Requires significant architectural changes
- Better browser compatibility and A/V sync

### Streaming AAC vs WebRTC (Pros/Cons)

**Streaming AAC (current approach)**
- **Pros**: Simple, low CPU vs. video, works in modern browsers/tools, small bandwidth
- **Cons**: No A/V sync, no NAT traversal, depends on browser AAC support

**WebRTC**
- **Pros**: Built-in A/V sync, low latency, NAT traversal, broad HomeKit/Scrypted compatibility
- **Cons**: Higher complexity, more CPU/RAM, requires STUN/TURN for remote access

**Recommendation**: Keep HTTP as MJPEG + companion AAC, use RTSP for A/V in Scrypted/HomeKit.

HTML test endpoint:
- `http://ESP32-IP/audio.wav` returns a short PCM WAV capture for browser testing.
- `http://ESP32-IP:81/audio.aac` is the continuous AAC stream.
- Mic enable/mute and sensitivity live in Feature Setup (Camera Config).

### 5a. Local Gong Playback (MAX98357A)

Goal: play a short "gong" sound locally when the doorbell is pressed.

Suggested format:
- **PCM 16-bit mono**, 8-16 kHz
- Store in **LittleFS** (`/gong.pcm`) or **PROGMEM** (short clip)
- Push samples to `I2S_NUM_1` TX (MAX98357A)
- Output enable/mute and volume live in Feature Setup (Core Features).
 - Default gong clip ships as `data/gong.pcm`.

Preparing a gong clip:
- Convert WAV to raw PCM: `python3 tools/convert_wav_to_pcm.py gong.wav data/gong.pcm`
- Or with ffmpeg: `ffmpeg -i gong.wav -ac 1 -ar 16000 -f s16le -acodec pcm_s16le data/gong.pcm`
- Upload with: `pio run -t uploadfs`

### 6. Session Structure Updates

Add audio fields to `RtspSession`:

```cpp
struct RtspSession {
    // Existing video fields...
    
    // Audio fields
    uint16_t audioSeq;
    uint32_t audioTimestamp;
    uint32_t audioSSRC;
    uint8_t interleavedAudioChannel;  // Usually 2 (RTP) and 3 (RTCP)
};
```

### 7. Performance Considerations

**CPU Usage**:
- I2S PDM capture is lightweight.
- AAC encoding is the heaviest step; load depends on bitrate/sample rate.
- Measure on-device (heap + loop timing) when tuning AAC settings.

**Memory Usage**:
- I2S DMA buffers: 4 × 512 samples × 2 bytes = **4 KB**
- Audio RTP buffer: ~200 bytes
- **Total**: ~5 KB additional RAM

**Network Bandwidth**:
- AAC @ 8 kHz: **16–32 kbps** + RTP/UDP overhead
- AAC @ 16 kHz: **24–48 kbps** + RTP/UDP overhead
- SIP RTP (G.711) remains **64 kbps** for intercom calls

### 8. Testing Plan

1. **I2S PDM Test**: Read raw PDM data, verify signal levels
2. **AAC Encode Test**: Validate AAC frames via `/audio.aac`
3. **RTSP Audio Test**: Stream audio-only to VLC/ffmpeg
4. **Combined A/V Test**: Stream audio+video to Scrypted
5. **HomeKit Integration Test**: Verify audio works in HomeKit doorbell
6. **Long-term Stability**: Monitor for memory leaks, audio sync issues

### 9. Implementation Steps

1. ✅ **Fix RTSP UDP streaming** (completed)
2. ✅ Add I2S PDM initialization (audio module)
3. ✅ Add audio capture path (on-demand for RTP + WAV endpoint)
4. ✅ Add AAC-LC encoder (ESP-ADF)
5. ✅ Update RTSP SDP to include AAC track
6. ✅ Implement AAC RTP packet sending (AU headers)
7. ✅ Add audio session fields to `RtspSession`
8. ⬜ Test with VLC/ffmpeg
9. ⬜ Test with Scrypted
10. ⬜ Test with HomeKit
11. ✅ Update documentation

### 10. References

- [ESP-IDF I2S PDM Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
- [RFC 3551 - RTP Audio/Video Profile](https://tools.ietf.org/html/rfc3551)
- [RFC 3640 - RTP Payload for MPEG-4 Audio](https://datatracker.ietf.org/doc/html/rfc3640)
- [Seeed XIAO ESP32-S3 Sense Schematic](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_SCH_v1.1.pdf)

## Status

**Current**: AAC-LC for RTSP + HTTP; SIP intercom remains G.711
**Next**: Validate audio in VLC/Scrypted/HomeKit and tune bitrate/sample rate
**Priority**: Medium (video streaming is primary, audio is enhancement)

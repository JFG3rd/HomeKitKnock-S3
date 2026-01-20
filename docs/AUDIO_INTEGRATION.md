<!--
 Project: HomeKitKnock-S3
 File: AUDIO_INTEGRATION.md
 Author: Jesse Greene
 -->

# Audio Integration Plan

This document outlines the plan to integrate the Seeed XIAO ESP32-S3 Sense onboard microphone into the HTTP and RTSP video streams.

## Current Implementation (v1.3.1)

- **RTSP audio**: PCMU/8000 is advertised only when the mic feature is enabled.
- **HTTP audio preview**: `/audio.wav` returns a short WAV clip for browser testing.
- **Continuous HTTP audio**: `http://ESP32-IP:81/audio` streams 16 kHz mono WAV as a companion to MJPEG.
- **Browser A/V page**: `http://ESP32-IP/live` pairs MJPEG + WAV (click-to-play audio).
- **Audio out**: MAX98357A I2S DAC plays gong/tone clips when enabled.
- **HTTP client limit**: MJPEG and WAV streams share the same max client cap.
- **Mic sharing**: RTSP and HTTP audio share the same mic capture path; avoid running both if audio stutters.
- **HTTP dependency**: WAV streaming is available only when HTTP camera streaming is enabled.
- **Compatibility**:
  - **Scrypted/HomeKit**: prefer RTSP for A/V (Scrypted can transcode).
  - **FRITZ!Box**: SIP is used for ringing only; HTTP audio is not consumed by the intercom.

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

#### Option A: PCM/G.711 μ-law (Recommended)
- **Codec**: G.711 μ-law (PCMU)
- **RTP Payload Type**: 0 (standard)
- **Bitrate**: 64 kbps (8 kHz, mono)
- **Pros**: Universal support, low CPU, simple implementation
- **Cons**: Lower quality than modern codecs
- **Note**: If the mic runs at 16 kHz, downsample to 8 kHz before μ-law encoding.

#### Option B: Opus (Best Quality)
- **Codec**: Opus
- **RTP Payload Type**: 111 (dynamic)
- **Bitrate**: 16-32 kbps (configurable)
- **Pros**: Excellent quality, low bitrate, HomeKit compatible
- **Cons**: Requires external library, higher CPU usage

**Decision**: Start with G.711 μ-law for simplicity, add Opus later if needed.

### 2a. HTML/Browser Audio Support

Browsers do not play raw G.711. For HTML views:
- **Preferred**: `audio/wav` (PCM 16-bit, mono, 8-16 kHz) via `/audio.wav`
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

// Audio stream (G.711 μ-law)
sdp += "m=audio 0 RTP/AVP 0\r\n";
sdp += "a=rtpmap:0 PCMU/8000\r\n";
sdp += "a=control:track2\r\n";
```

Implementation notes:
- Audio is advertised only when mic is enabled in Feature Setup.
- Track mapping: `track1` = video, `track2` = audio (PCMU/8000).

#### RTP Audio Packet Format

G.711 μ-law RTP packet structure:
```
[RTP Header 12 bytes] + [G.711 samples]
```

RTP header for audio:
- **Version**: 2
- **Payload Type**: 0 (PCMU)
- **Sequence Number**: Incremented per packet
- **Timestamp**: Sample count (8000 Hz clock)
- **SSRC**: Unique audio stream identifier

Sample code:
```cpp
void sendAudioRtp(RtspSession* session, const int16_t* pcm, size_t samples) {
    // Convert PCM to G.711 μ-law
    uint8_t ulaw[samples];
    for (size_t i = 0; i < samples; i++) {
        ulaw[i] = linear2ulaw(pcm[i]);
    }
    
    // Build RTP packet
    uint8_t rtpPacket[12 + samples];
    rtpPacket[0] = 0x80;  // V=2, P=0, X=0, CC=0
    rtpPacket[1] = 0x00;  // M=0, PT=0 (PCMU)
    
    // Sequence number (16-bit, big-endian)
    rtpPacket[2] = (session->audioSeq >> 8) & 0xFF;
    rtpPacket[3] = session->audioSeq & 0xFF;
    
    // Timestamp (32-bit, big-endian, 8000 Hz clock)
    rtpPacket[4] = (session->audioTimestamp >> 24) & 0xFF;
    rtpPacket[5] = (session->audioTimestamp >> 16) & 0xFF;
    rtpPacket[6] = (session->audioTimestamp >> 8) & 0xFF;
    rtpPacket[7] = session->audioTimestamp & 0xFF;
    
    // SSRC (32-bit, big-endian)
    rtpPacket[8] = (session->audioSSRC >> 24) & 0xFF;
    rtpPacket[9] = (session->audioSSRC >> 16) & 0xFF;
    rtpPacket[10] = (session->audioSSRC >> 8) & 0xFF;
    rtpPacket[11] = session->audioSSRC & 0xFF;
    
    // Copy μ-law samples
    memcpy(rtpPacket + 12, ulaw, samples);
    
    // Send packet (TCP or UDP)
    if (session->useTcpInterleaved) {
        sendTcpInterleaved(session, session->interleavedAudioChannel, rtpPacket, 12 + samples);
    } else {
        session->rtpSocket.beginPacket(session->clientIP, session->rtpPort + 2);  // Audio on +2
        session->rtpSocket.write(rtpPacket, 12 + samples);
        session->rtpSocket.endPacket();
    }
    
    session->audioSeq++;
    session->audioTimestamp += samples;  // 8000 Hz clock
}
```

### 4. G.711 μ-law Encoder

Simple PCM to μ-law conversion:

```cpp
// G.711 μ-law encoding table
static const int16_t seg_end[8] = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};

uint8_t linear2ulaw(int16_t pcm) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;
    
    // Get sign and magnitude
    if (pcm < 0) {
        pcm = -pcm;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    
    // Clip to 14-bit signed range
    if (pcm > 0x3FFF) pcm = 0x3FFF;
    
    // Convert to μ-law
    pcm += 0x84;  // Bias
    for (seg = 0; seg < 8; seg++) {
        if (pcm <= seg_end[seg]) break;
    }
    
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    
    uval = (seg << 4) | ((pcm >> (seg + 3)) & 0x0F);
    return (uint8_t)(uval ^ mask);
}
```

### 5. HTTP Stream Integration

HTTP MJPEG streams don't natively support audio. Options:

#### Option A: Separate Audio Stream (Implemented)
- Continuous WAV stream at `http://ESP32-IP:81/audio`
- Short WAV preview at `http://ESP32-IP/audio.wav`
- MJPEG remains at `http://ESP32-IP:81/stream` (pair manually or use `/live`)
- Clients must sync audio/video manually

#### Option B: WebRTC/HLS (Planned)
- Use WebRTC or HLS for combined A/V streaming
- Requires significant architectural changes
- Better browser compatibility and A/V sync

### Streaming WAV vs WebRTC (Pros/Cons)

**Streaming WAV (current approach)**
- **Pros**: Simple, low CPU, easy to debug, works for local browsers and tools
- **Cons**: No A/V sync, no NAT traversal, limited client support for continuous WAV

**WebRTC**
- **Pros**: Built-in A/V sync, low latency, NAT traversal, broad HomeKit/Scrypted compatibility
- **Cons**: Higher complexity, more CPU/RAM, requires STUN/TURN for remote access

**Recommendation**: Keep HTTP as video-only, use RTSP for audio+video.

HTML test endpoint:
- `http://ESP32-IP/audio.wav` returns a short PCM WAV capture for browser testing.
- Mic enable/mute and sensitivity live in Feature Setup (Scrypted Stream Options).

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
- I2S PDM reading: ~2-3% CPU @ 16 kHz
- G.711 encoding: ~1% CPU
- RTP packetization: <1% CPU
- **Total**: ~4-5% additional CPU load

**Memory Usage**:
- I2S DMA buffers: 4 × 512 samples × 2 bytes = **4 KB**
- Audio RTP buffer: ~200 bytes
- **Total**: ~5 KB additional RAM

**Network Bandwidth**:
- G.711 @ 8 kHz: **64 kbps**
- RTP overhead: ~5 kbps
- **Total**: ~70 kbps additional (acceptable for WiFi)

### 8. Testing Plan

1. **I2S PDM Test**: Read raw PDM data, verify signal levels
2. **G.711 Encoding Test**: Convert PCM to μ-law, check waveform
3. **RTSP Audio Test**: Stream audio-only to VLC/ffmpeg
4. **Combined A/V Test**: Stream audio+video to Scrypted
5. **HomeKit Integration Test**: Verify audio works in HomeKit doorbell
6. **Long-term Stability**: Monitor for memory leaks, audio sync issues

### 9. Implementation Steps

1. ✅ **Fix RTSP UDP streaming** (completed)
2. ✅ Add I2S PDM initialization (audio module)
3. ✅ Add audio capture path (on-demand for RTP + WAV endpoint)
4. ✅ Implement G.711 μ-law encoder
5. ✅ Update RTSP SDP to include audio track
6. ✅ Implement audio RTP packet sending
7. ✅ Add audio session fields to `RtspSession`
8. ⬜ Test with VLC/ffmpeg
9. ⬜ Test with Scrypted
10. ⬜ Test with HomeKit
11. ✅ Update documentation

### 10. References

- [ESP-IDF I2S PDM Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
- [RFC 3551 - RTP Audio/Video Profile](https://tools.ietf.org/html/rfc3551)
- [RFC 5391 - RTP Payload Format for ITU-T G.711.1](https://tools.ietf.org/html/rfc5391)
- [Seeed XIAO ESP32-S3 Sense Schematic](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_SCH_v1.1.pdf)

## Status

**Current**: Mic + gong scaffolding wired; RTSP audio track sending PCMU
**Next**: Validate audio in VLC/Scrypted/HomeKit and tune capture pacing
**Priority**: Medium (video streaming is primary, audio is enhancement)

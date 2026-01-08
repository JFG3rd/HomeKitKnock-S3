<!--
 Project: HomeKitKnock-S3
 File: docs/SIP_INTEGRATION.md
 Author: Jesse Greene
 -->

# SIP Client Integration Guide

## Overview
Integrated SIP client functionality into the ESP32-S3 doorbell to ring FRITZ!Box internal phones (DECT handsets) when the doorbell button is pressed. The client now supports SIP Digest authentication challenges (401/407).

## What Was Changed

### New Files Created
1. **`include/sip_client.h`** - SIP client header with function declarations
2. **`src/sip_client.cpp`** - SIP client implementation with REGISTER, INVITE, and CANCEL support

### Modified Files
1. **`src/main.cpp`**
   - Added `#include "sip_client.h"`
   - Declared global `SipConfig sipConfig`
   - Added SIP initialization in `setup()`
   - Replaced TR-064 ring logic with SIP in `handleDoorbellPress()`
   - Added periodic SIP REGISTER in `loop()`
   - Added `/sip` setup page endpoint
   - Added `/saveSIP` POST endpoint for saving configuration
   - Added `/sipDebug` GET endpoint for debugging
   - Added `/ring/sip` GET endpoint for testing
   - Updated main page UI to show SIP status and link to SIP setup

## How It Works

### SIP Registration Flow
1. ESP32 sends SIP REGISTER to FRITZ!Box every 60 seconds
2. FRITZ!Box may challenge with Digest auth (401/407); ESP32 retries with Authorization
3. FRITZ!Box acknowledges registration (ESP32 appears as an IP phone)
3. ESP32 listens for SIP responses on UDP port 5062

### Doorbell Ring Flow
1. User presses doorbell button (GPIO pin)
2. `handleDoorbellPress()` calls `triggerSipRing(sipConfig)`
3. ESP32 sends SIP INVITE to configured target (e.g., **610)
4. Rings for 2.5 seconds while processing responses
5. ESP32 sends SIP CANCEL to stop ringing

### Configuration Storage
- SIP credentials stored in NVS under "sip" namespace:
  - `sip_user` - SIP username (e.g., 620)
  - `sip_password` - SIP password
  - `sip_displayname` - Caller ID name (default: "Doorbell")
  - `sip_target` - Target number (default: "**610")
  - `scrypted_webhook` - Scrypted doorbell webhook URL (optional)

## Setup Instructions

### 1. Create IP Phone in FRITZ!Box
1. Navigate to **Telefonie → Telefoniegeräte**
2. Click **"Neues Gerät einrichten"**
3. Select **"Telefon (mit und ohne Anrufbeantworter)"**
4. Select **"LAN/WLAN (IP-Telefon)"**
5. Enter credentials:
   - **Username**: e.g., 620
   - **Password**: Create a secure password
   - **Name**: ESP32-Doorbell
6. Save the configuration

### 2. Configure ESP32 Doorbell
1. Upload firmware to ESP32
2. Connect to doorbell's WiFi network
3. Navigate to `http://<ESP32-IP>/sip` in browser
4. Enter SIP credentials:
   - **SIP Username**: The username from step 1 (e.g., 620)
   - **SIP Password**: The password from step 1
   - **Display Name**: "Doorbell" (or custom name)
   - **Target Number**: 
     - `**610` to ring all DECT phones
     - `**9` + extension to ring specific phone
5. Click **"💾 Save"**
6. Test with **"🔔 Test SIP Ring"** button

### 3. Verify Registration
- Check FRITZ!Box → Telefonie → Telefoniegeräte
- ESP32-Doorbell should show as "Registered" (green)
- ESP32 serial monitor should show SIP REGISTER messages every 60 seconds

## Testing

### Web UI Test
1. Navigate to `http://<ESP32-IP>`
2. Click **"🔔 Test Ring (SIP)"** button
3. Internal phones should ring for 2.5 seconds

### Debug Information
1. Navigate to `http://<ESP32-IP>/sipDebug`
2. JSON response shows:
   ```json
   {
     "sip_user": "620",
     "sip_target": "**610",
     "has_sip_config": true
   }
   ```

### Serial Monitor
Monitor SIP messages:
```
---- SIP REGISTER ----
REGISTER sip:fritz.box SIP/2.0
...

---- SIP INVITE ----
INVITE sip:**610@fritz.box SIP/2.0
...

---- SIP CANCEL ----
CANCEL sip:**610@fritz.box SIP/2.0
...

---- SIP RX ----
SIP/2.0 200 OK
...
```

## Troubleshooting

### SIP Registration Fails
- **Check credentials**: Verify username/password match FRITZ!Box IP phone configuration
- **Check network**: Ensure ESP32 can reach 192.168.178.1:5060 (UDP)
- **Check FRITZ!Box logs**: Telefonie → Anrufliste shows SIP registration attempts
- **Auth challenge**: If you see 401/407 without a follow-up REGISTER, verify digest auth support is enabled (current firmware supports it)

### Ring Doesn't Work
- **Verify registration**: ESP32 must be registered before ringing works
- **Check target number**: Ensure `**610` or configured target is valid
- **Check SIP debug**: Visit `/sipDebug` to verify configuration
- **Serial monitor**: Look for "SIP ring failed" messages

### ESP32 Shows as Offline in FRITZ!Box
- **Registration expired**: ESP32 sends REGISTER every 60 seconds with 120s expiry
- **Network issue**: Check WiFi connection and gateway reachability
- **Restart ESP32**: Power cycle or use `/forget` endpoint to reset WiFi

## Key Features

### Advantages Over TR-064/HTTP
- ✅ **Works on all FRITZ!Box models** (TR-064 dialing disabled on 6591 Cable)
- ✅ **No authentication issues** (simple SIP credentials)
- ✅ **Standard SIP protocol** (RFC 3261 compliant)
- ✅ **Real IP phone** (appears as genuine device in FRITZ!Box)

### Maintained Compatibility
- Existing WiFi provisioning system intact
- Camera integration still works (MJPEG streaming)
- Web UI with dark mode preserved
- TR-064 setup page still available (for reference)

## Technical Details

### SIP Protocol Implementation
- **Transport**: UDP port 5062 (local) → 5060 (FRITZ!Box)
- **Messages**: REGISTER, INVITE, CANCEL
- **Timing**: 
  - REGISTER every 60 seconds (120s expiry)
  - INVITE ring duration: 2.5 seconds
  - CANCEL sent after ring duration

### Network Configuration
- **Domain**: fritz.box
- **Proxy**: fritz.box:5060
- **Local IP**: Obtained from WiFi DHCP
- **User-Agent**: ESP32-Doorbell/1.0

### Memory Usage
- **RAM**: 15.8% (51,732 bytes)
- **Flash**: 33.7% (1,127,653 bytes)
- **Build**: Successfully compiled with no errors

## Next Steps

### Upload and Test
```bash
# Upload firmware
platformio run -t upload

# Monitor serial output
platformio device monitor
```

### Verify Operation
1. Watch serial monitor for SIP REGISTER messages
2. Check FRITZ!Box shows ESP32 as registered IP phone
3. Press doorbell button or use web UI test button
4. Verify internal phones ring for 2.5 seconds

## Files Modified Summary

| File | Changes |
|------|---------|
| `include/sip_client.h` | New: SIP client interface |
| `src/sip_client.cpp` | New: SIP protocol implementation |
| `src/main.cpp` | Modified: Added SIP integration, UI updates |

## Configuration Migration

Old TR-064 config (stored in NVS):
- Namespace: `tr064`
- Keys: `tr064_user`, `tr064_pass`, `number`

New SIP config:
- Namespace: `sip`
- Keys: `sip_user`, `sip_password`, `sip_displayname`, `sip_target`

Both configurations coexist - SIP is now the primary ring method.

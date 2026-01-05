# TR-064 FRITZ!Box Ring Issue - Debugging Guide

## Problem
TR-064 authentication works (FRITZ!Box logs show successful login), but the ring action fails with:
```
[TR064][httpRequest] <TR064> Failed, message: ''
[TR064][action]<error> Request Failed
```

## Changes Made

### Fixed Service URN
Changed from:
```cpp
connection.action("X_VoIP:1", ...)
```

To proper URN format:
```cpp
connection.action("urn:dslforum-org:service:X_VoIP:1", ...)
```

### Added Verbose Debugging
- Set `connection.debug_level = connection.DEBUG_VERBOSE`
- Added serial output for each step
- Shows router IP, port, and number being dialed
- Indicates which attempt (primary/fallback) succeeded or failed

## Upload Instructions

1. **Close Serial Monitor** (port is currently busy)
2. **Upload firmware**:
   ```bash
   pio run -t upload
   ```
3. **Open Serial Monitor**:
   ```bash
   pio device monitor
   ```
4. **Test ring button** and observe detailed output

## Expected Debug Output

You should now see:
```
📞 TR-064: Dialing **9 on 192.168.178.1:49000
📞 TR-064: Attempting X_AVM-DE_DialNumber action...
[TR064][init] ...
[TR064][action] ...
✅ TR-064: Ring triggered successfully
```

Or if it fails, more detailed error information from the TR-064 library.

## Common Issues & Solutions

### 1. Wrong Internal Number Format
- **Try**: `**9` (all FRITZ!Fons)
- **Or**: `**610` (specific extension)
- **Or**: `1` (handset 1), `2` (handset 2), etc.

### 2. Service Not Available
Some FRITZ!Box models/firmware versions may require:
- Different service name (check FRITZ!Box TR-064 service list at `http://192.168.178.1:49000/tr64desc.xml`)
- Different action name

### 3. Permission Issues
Verify the TR-064 user has:
- ✅ "FRITZ!Box Einstellungen" permission
- ✅ "Telefonie" permission
- ✅ TR-064 access enabled in FRITZ!Box settings

### 4. Alternative Action
If `X_AVM-DE_DialNumber` doesn't work, you may need to use:
- `X_AVM-DE_DialSetConfig` (for setting up dial rules)
- Check the FRITZ!Box service description for available actions

## Testing Steps

1. Upload the new firmware
2. Click "Test Ring" button
3. Check Serial Monitor for detailed output
4. Check FRITZ!Box call log (Telefonie → Anrufliste)
5. If still failing, share the full verbose TR-064 debug output

## FRITZ!Box TR-064 Documentation

Service description available at:
```
http://192.168.178.1:49000/tr64desc.xml
```

This XML file lists all available services and their control URLs.

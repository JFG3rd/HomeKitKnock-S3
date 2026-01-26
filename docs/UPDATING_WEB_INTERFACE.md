<!--
 Project: HomeKitKnock-S3
 File: docs/UPDATING_WEB_INTERFACE.md
 Author: Jesse Greene
 -->

# Updating the Web Interface

## Important: Uploading Changes

When you modify files in the `data/` directory (like `style.css`), you **must upload the filesystem** to the ESP32 for changes to take effect.

### Upload Filesystem Command

```bash
pio run -t uploadfs
```

Or from VS Code:
1. Click on PlatformIO icon in sidebar
2. Find "seeed_xiao_esp32s3" environment
3. Click "Platform" → "Upload Filesystem Image"

### Files That Require Filesystem Upload

- `data/style.css` - Main stylesheet
- `data/favicon.ico` - Website icon
- Any other static files in the `data/` directory

### Files That Don't Require Filesystem Upload

- `src/main.cpp` - Main firmware code (use regular upload)
- `include/*.h` - Header files (use regular upload)

## Development Workflow

1. Make changes to `data/style.css` or other static files
2. Run `pio run -t uploadfs` to upload filesystem
3. Refresh the web browser (Ctrl+F5 or Cmd+Shift+R for hard refresh)
4. If changes still don't appear, check browser DevTools → Network tab to verify CSS is loading

## CSS Layout Notes

### Three-Card Layout
- Cards display side-by-side on screens ≥980px wide
- Cards stack vertically on screens <980px wide
- Each card has `flex: 1 1 0` to ensure equal width

### Button Styles
- Base buttons: `.button` (theme-driven)
- `.btn-save`: Save/apply/upload actions
- `.btn-info`: Logs, debug, and informational links
- `.btn-nav`: Navigation/back/setup links
- `.btn-test`: Test or diagnostic actions
- `.danger-btn`: Destructive actions (reset/forget/reboot)

## Troubleshooting

**Problem**: CSS changes don't appear after uploading firmware
**Solution**: Run `pio run -t uploadfs` to update the filesystem

**Problem**: Three cards are stacked instead of side-by-side
**Solution**: 
1. Check browser window width (must be ≥1001px)
2. Hard refresh the page (Ctrl+F5 / Cmd+Shift+R)
3. Verify CSS loaded in DevTools

**Problem**: Buttons look flat/unstyled
**Solution**: Clear browser cache and hard refresh

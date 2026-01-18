/*
 * Project: HomeKitKnock-S3
 * File: include/version_info.h
 * Author: Jesse Greene
 */

#ifndef VERSION_INFO_H
#define VERSION_INFO_H

// Build-time identifiers injected by tools/pio_version.py.
// Fallbacks keep the firmware usable even without git metadata.
#ifndef FW_VERSION
#define FW_VERSION "0.0.0+nogit"
#endif

#ifndef FW_BUILD_TIME
#define FW_BUILD_TIME "unknown"
#endif

#endif // VERSION_INFO_H

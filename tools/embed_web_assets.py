#!/usr/bin/env python3
"""
Embed web assets (HTML, CSS) from data/ directory into C++ header files.
Converts files to PROGMEM byte arrays for serving from embedded firmware.

Usage:
    python3 embed_web_assets.py <data_dir> <output_dir>
"""

import os
import sys
import gzip
from pathlib import Path

def compress_content(content):
    """Compress content using gzip and return compressed bytes."""
    return gzip.compress(content.encode('utf-8') if isinstance(content, str) else content)

def to_hex_string(data, bytes_per_line=16):
    """Convert bytes to hex string formatted for C arrays."""
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i:i+bytes_per_line]
        hex_bytes = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f"    {hex_bytes}")
    return ',\n'.join(lines)

def generate_header(filename, compressed_data, original_size):
    """Generate C++ header content for embedded file."""
    var_name = Path(filename).stem.replace('-', '_').replace('.', '_')
    size = len(compressed_data)
    
    header = f"""// Auto-generated embedded web asset: {filename}
// Original size: {original_size} bytes, Compressed: {size} bytes

#pragma once

#include <stdint.h>
#include <stddef.h>

// Embedded {filename} (gzip compressed)
const uint8_t PROGMEM {var_name}_data[] = {{
{to_hex_string(compressed_data)}
}};

const size_t {var_name}_size = {size};
const size_t {var_name}_original_size = {original_size};
const char {var_name}_mime[] PROGMEM = "{get_mime_type(filename)}";
"""
    return header

def get_mime_type(filename):
    """Get MIME type for file."""
    ext = Path(filename).suffix.lower()
    mime_types = {
        '.html': 'text/html; charset=utf-8',
        '.css': 'text/css; charset=utf-8',
        '.js': 'application/javascript; charset=utf-8',
        '.json': 'application/json',
        '.svg': 'image/svg+xml',
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.ico': 'image/x-icon',
    }
    return mime_types.get(ext, 'application/octet-stream')

def embed_web_assets(data_dir, output_dir):
    """Convert web assets in data_dir to embedded headers in output_dir."""
    data_path = Path(data_dir)
    output_path = Path(output_dir)
    
    if not data_path.exists():
        print(f"Error: Data directory not found: {data_dir}")
        return False
    
    output_path.mkdir(parents=True, exist_ok=True)
    
    # Files to embed
    embed_files = ['index.html', 'style.css', 'setup.html', 'wifi-setup.html', 
                   'live.html', 'guide.html', 'ota.html', 'sip.html', 
                   'tr064.html', 'logs-doorbell.html', 'logs-camera.html']
    
    files_data = {}
    
    for filename in embed_files:
        file_path = data_path / filename
        if not file_path.exists():
            print(f"Warning: Skipping {filename} - not found")
            continue
        
        print(f"Embedding {filename}...")
        
        try:
            with open(file_path, 'rb') as f:
                content = f.read()
            
            compressed = compress_content(content)
            original_size = len(content)
            
            # Generate header
            header_content = generate_header(filename, compressed, original_size)
            
            # Write individual header
            header_file = output_path / f"embedded_{Path(filename).stem}.h"
            with open(header_file, 'w') as f:
                f.write(header_content)
            
            files_data[filename] = {
                'var_name': Path(filename).stem.replace('-', '_').replace('.', '_'),
                'original_size': original_size,
                'compressed_size': len(compressed),
                'mime_type': get_mime_type(filename)
            }
            
            print(f"  ✓ {filename}: {original_size} → {len(compressed)} bytes")
        
        except Exception as e:
            print(f"  ✗ Error processing {filename}: {e}")
            return False
    
    # Generate master header with file registry
    master_header = generate_master_header(files_data)
    master_file = output_path / "embedded_web_assets.h"
    with open(master_file, 'w') as f:
        f.write(master_header)
    
    print(f"\n✓ Generated {len(files_data)} embedded assets")
    print(f"  Output: {output_path}/")
    return True

def generate_master_header(files_data):
    """Generate master header with registry of all embedded files."""
    includes = '\n'.join([f'#include "embedded_{f.replace(".html", "").replace(".css", "")}.h"' 
                          for f in files_data.keys()])
    
    entries = []
    for filename, data in files_data.items():
        var_name = data['var_name']
        entries.append(f'    {{"{filename}", {var_name}_data, {var_name}_size, {var_name}_mime}}')
    
    registry = ',\n'.join(entries)
    
    header = f"""// Auto-generated master header for embedded web assets
// This file includes all embedded web assets and provides a registry

#pragma once

#include <stdint.h>
#include <stddef.h>

{includes}

// File registry entry
struct EmbeddedFile {{
    const char* filename;
    const uint8_t* data;
    size_t size;
    const char* mime_type;
}};

// Registry of all embedded files
const EmbeddedFile PROGMEM embedded_files[] = {{
{registry}
}};

const size_t embedded_files_count = {len(files_data)};

// Helper function to find file by name
inline const EmbeddedFile* find_embedded_file(const char* filename) {{
    for (size_t i = 0; i < embedded_files_count; i++) {{
        if (strcmp_P(filename, (const char*)pgm_read_ptr(&embedded_files[i].filename)) == 0) {{
            return &embedded_files[i];
        }}
    }}
    return nullptr;
}}
"""
    return header

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python3 embed_web_assets.py <data_dir> <output_dir>")
        sys.exit(1)
    
    data_dir = sys.argv[1]
    output_dir = sys.argv[2]
    
    success = embed_web_assets(data_dir, output_dir)
    sys.exit(0 if success else 1)

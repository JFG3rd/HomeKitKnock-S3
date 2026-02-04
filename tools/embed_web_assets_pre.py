#!/usr/bin/env python3
"""
PlatformIO pre-build script to embed web assets.
Converts HTML/CSS from data/ to embedded C++ headers before compilation.
"""

import os
import sys
import subprocess
from pathlib import Path

Import("env")

# Get project directories
project_dir = env.get("PROJECT_DIR")
data_dir = os.path.join(project_dir, "data")
include_dir = os.path.join(project_dir, "include")
script_dir = os.path.join(project_dir, "tools")
embed_script = os.path.join(script_dir, "embed_web_assets.py")

def embed_web_assets_callback(source, target, env):
    """Callback to run embedding before build."""
    print("\n" + "="*60)
    print("Embedding web assets from data/ directory...")
    print("="*60)

    if not os.path.exists(embed_script):
        print(f"Error: embed_web_assets.py not found at {embed_script}")
        env.Exit(1)

    if not os.path.exists(data_dir):
        print(f"Warning: data/ directory not found at {data_dir}")
        print("Web assets will not be embedded")
        return

    try:
        # Detect if building for ESP-IDF (no Arduino framework)
        frameworks = env.get("PIOFRAMEWORK", [])
        is_idf_only = "espidf" in frameworks and "arduino" not in frameworks

        # Build command with --idf flag if needed
        cmd = [sys.executable, embed_script, data_dir, include_dir]
        if is_idf_only:
            cmd.append("--idf")
            print("Detected ESP-IDF build - generating C-compatible headers")

        # Run the embedding script
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30
        )
        
        print(result.stdout)
        if result.stderr:
            print("Errors:", result.stderr)
        
        if result.returncode != 0:
            print(f"Error: embed_web_assets.py failed with code {result.returncode}")
            env.Exit(1)
        
        print("✓ Web assets embedded successfully\n")
    
    except subprocess.TimeoutExpired:
        print("Error: embed_web_assets.py timeout")
        env.Exit(1)
    except Exception as e:
        print(f"Error: Failed to run embed_web_assets.py: {e}")
        env.Exit(1)

# Register the pre-build callback
# Use main.c.o for ESP-IDF, main.cpp.o for Arduino
frameworks = env.get("PIOFRAMEWORK", [])
is_idf_only = "espidf" in frameworks and "arduino" not in frameworks

if is_idf_only:
    env.AddPreAction("$BUILD_DIR/src/main.c.o", embed_web_assets_callback)
else:
    env.AddPreAction("$BUILD_DIR/src/main.cpp.o", embed_web_assets_callback)

print("✓ Pre-build script registered: web assets will be embedded before compilation")

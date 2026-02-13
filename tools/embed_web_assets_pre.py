#!/usr/bin/env python3
"""
PlatformIO pre-build script to embed web assets.
Converts HTML/CSS from data/ to embedded C headers before compilation.
"""

import os
import sys
import subprocess

Import("env")

# Get project directories
project_dir = env.get("PROJECT_DIR")
data_dir = os.path.join(project_dir, "data")
include_dir = os.path.join(project_dir, "include")
script_dir = os.path.join(project_dir, "tools")
embed_script = os.path.join(script_dir, "embed_web_assets.py")

def run_embed_web_assets(trigger_label):
    """Run embedding before compilation starts."""
    print("\n" + "="*60)
    print(f"Embedding web assets from data/ directory ({trigger_label})...")
    print("="*60)

    if not os.path.exists(embed_script):
        print(f"Error: embed_web_assets.py not found at {embed_script}")
        env.Exit(1)

    if not os.path.exists(data_dir):
        print(f"Warning: data/ directory not found at {data_dir}")
        print("Web assets will not be embedded")
        return

    try:
        cmd = [sys.executable, embed_script, data_dir, include_dir, "--idf"]

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

# Critical: run immediately when this pre-script loads, before any compilation.
run_embed_web_assets("pre-script startup")

print("✓ Pre-build script active: assets embedded once at startup before compile")

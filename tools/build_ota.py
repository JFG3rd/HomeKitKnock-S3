#!/usr/bin/env python3
"""
Small helper to build and export OTA-ready binaries.
- Builds firmware and LittleFS images for the target PlatformIO environment.
- Renames the artifacts to include the version string for easier OTA uploads.
- Drops the outputs into dist/ota so they are easy to find.
"""

import argparse
import configparser
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENV = "seeed_xiao_esp32s3"
DEFAULT_PREFIX = "XIAOS3Sense"


def load_version(platformio_path: Path, env: str) -> str:
    """Read custom_fw_version from platformio.ini for the given env."""
    config = configparser.ConfigParser()
    config.optionxform = str  # Preserve case for keys
    if not config.read(platformio_path):
        raise FileNotFoundError(f"platformio.ini not found at {platformio_path}")
    try:
        return config[f"env:{env}"]["custom_fw_version"].strip()
    except KeyError as exc:  # pragma: no cover - simple guardrail
        raise KeyError("custom_fw_version not set in platformio.ini") from exc


def run_cmd(cmd: list[str]) -> None:
    """Run a command in the repo root and stream output."""
    subprocess.run(cmd, cwd=ROOT, check=True)


def copy_artifact(src: Path, dest: Path) -> None:
    """Copy a build artifact to the dist/ota folder, creating it if needed."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def build_and_export(env: str, prefix: str, version: str, skip_firmware: bool, skip_fs: bool) -> None:
    build_dir = ROOT / ".pio" / "build" / env

    if not skip_firmware:
        print(f"[firmware] Building env {env}...")
        run_cmd(["pio", "run", "-e", env])
        firmware_src = build_dir / "firmware.bin"
        firmware_dest = ROOT / "dist" / "ota" / f"{prefix}-{version}-firmware.bin"
        copy_artifact(firmware_src, firmware_dest)
        print(f"[firmware] Exported to {firmware_dest}")

    if not skip_fs:
        print(f"[littlefs] Building filesystem for env {env}...")
        run_cmd(["pio", "run", "-t", "buildfs", "-e", env])
        littlefs_src = build_dir / "littlefs.bin"
        littlefs_dest = ROOT / "dist" / "ota" / f"{prefix}-{version}-littlefs.bin"
        copy_artifact(littlefs_src, littlefs_dest)
        print(f"[littlefs] Exported to {littlefs_dest}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build OTA firmware and filesystem images.")
    parser.add_argument("--env", default=DEFAULT_ENV, help="PlatformIO environment to build")
    parser.add_argument("--prefix", default=DEFAULT_PREFIX, help="File prefix used for exported artifacts")
    parser.add_argument(
        "--version",
        help="Override firmware version. Defaults to custom_fw_version from platformio.ini",
    )
    parser.add_argument("--skip-firmware", action="store_true", help="Skip building firmware.bin")
    parser.add_argument("--skip-fs", action="store_true", help="Skip building littlefs.bin")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    version = args.version or load_version(ROOT / "platformio.ini", args.env)
    build_and_export(args.env, args.prefix, version, args.skip_firmware, args.skip_fs)


if __name__ == "__main__":
    main()

import datetime
import subprocess

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
project_dir = env["PROJECT_DIR"]


def run_git(args):
    return subprocess.check_output(
        ["git"] + args,
        cwd=project_dir,
        stderr=subprocess.DEVNULL,
    ).decode("utf-8").strip()


def read_version_base():
    custom_version = env.GetProjectOption("custom_fw_version")
    if custom_version:
        return custom_version.lstrip("v").strip()

    try:
        tag = run_git(["describe", "--tags", "--abbrev=0"])
        return tag.lstrip("v")
    except Exception:
        return "0.0.0"


def read_git_hash():
    try:
        return run_git(["rev-parse", "--short", "HEAD"])
    except Exception:
        return "nogit"


version_base = read_version_base()
git_hash = read_git_hash()
fw_version = f"{version_base}+{git_hash}"
build_time = datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC")

env.Append(
    CPPDEFINES=[
        ("FW_VERSION", f'\\"{fw_version}\\"'),
        ("FW_BUILD_TIME", f'\\"{build_time}\\"'),
    ]
)

print(f"Firmware version: {fw_version}")
print(f"Firmware build time: {build_time}")

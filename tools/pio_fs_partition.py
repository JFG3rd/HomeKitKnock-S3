import os

from SCons.Script import COMMAND_LINE_TARGETS, DefaultEnvironment

# PlatformIO's espressif32 builder only recognizes filesystem subtypes by string.
# IDF 4.4 needs LittleFS to be numeric (0x83), so patch the builder to accept 0x83
# when using uploadfs/buildfs targets.

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def patch_espressif32_builder():
    env = DefaultEnvironment()
    platform_dir = env.PioPlatform().get_dir()
    builder_main = os.path.join(platform_dir, "builder", "main.py")
    if not os.path.isfile(builder_main):
        return

    with open(builder_main, "r", encoding="utf-8") as handle:
        content = handle.read()

    needle = 'p["subtype"] in ("spiffs", "fat", "littlefs")'
    if needle not in content:
        return

    patched = content.replace(
        needle,
        'p["subtype"] in ("spiffs", "fat", "littlefs", "0x83")',
        1,
    )

    if patched == content:
        return

    with open(builder_main, "w", encoding="utf-8") as handle:
        handle.write(patched)


if FS_TARGETS & set(COMMAND_LINE_TARGETS):
    patch_espressif32_builder()

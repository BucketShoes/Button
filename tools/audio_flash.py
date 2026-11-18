# PlatformIO extra script: optionally writes the audio partition as part of the
# normal Upload, so it goes to the same port the firmware does (the VS Code port
# switcher only passes --upload-port to the built-in Upload/Monitor/Test tasks,
# not to custom targets).
#
# Controlled by custom_audio in platformio.ini:
#   keep    leave the audio partition alone (default, fast uploads)
#   upload  also write audio/test_audio.bin
#   erase   also blank the clip header, so the firmware sees no clip
Import("env")  # noqa: F821  (provided by PlatformIO)

import os
from SCons.Script import COMMAND_LINE_TARGETS  # noqa: E402

AUDIO_OFFSET = "0x310000"  # must match partitions.csv
AUDIO_SIZE = 0x400000

mode = env.GetProjectOption("custom_audio", "keep").strip().lower()  # noqa: F821

if "upload" in COMMAND_LINE_TARGETS and mode != "keep":
    if mode == "upload":
        img = os.path.join(env.subst("$PROJECT_DIR"), "audio", "test_audio.bin")  # noqa: F821
        if not os.path.isfile(img):
            raise SystemExit("custom_audio = upload, but %s is missing (see README)" % img)
        if os.path.getsize(img) > AUDIO_SIZE:
            raise SystemExit("%s is larger than the 4 MB audio partition" % img)
    elif mode == "erase":
        build_dir = env.subst("$BUILD_DIR")  # noqa: F821
        os.makedirs(build_dir, exist_ok=True)
        img = os.path.join(build_dir, "audio_erase.bin")
        with open(img, "wb") as f:
            f.write(b"\xff" * 4096)
    else:
        raise SystemExit("custom_audio must be keep, upload or erase (got %r)" % mode)
    print("custom_audio = %s: also writing %s at %s" % (mode, img, AUDIO_OFFSET))
    env.Append(UPLOADERFLAGS=[AUDIO_OFFSET, img])  # noqa: F821

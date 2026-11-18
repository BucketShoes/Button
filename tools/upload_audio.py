# PlatformIO extra script: adds an "Upload audio" custom target that writes
# audio/test_audio.bin into the "audio" partition (see partitions.csv).
# Normal firmware uploads leave that partition alone.
Import("env")  # noqa: F821  (provided by PlatformIO)

import os
import subprocess

AUDIO_OFFSET = "0x310000"
AUDIO_SIZE = 0x400000


def upload_audio(target, source, env):
    img = os.path.join(env.subst("$PROJECT_DIR"), "audio", "test_audio.bin")
    if not os.path.isfile(img):
        print("missing %s (make it with tools/make_test_audio.py)" % img)
        return 1
    if os.path.getsize(img) > AUDIO_SIZE:
        print("%s is larger than the 4 MB audio partition" % img)
        return 1
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    cmd = [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32c6"]
    port = env.subst("$UPLOAD_PORT")
    if port:
        cmd += ["--port", port]
    cmd += ["write-flash", AUDIO_OFFSET, img]
    print(" ".join(cmd))
    return subprocess.call(cmd)


env.AddCustomTarget(  # noqa: F821
    name="upload_audio",
    dependencies=None,
    actions=[upload_audio],
    title="Upload audio",
    description="Flash audio/test_audio.bin to the audio partition",
)

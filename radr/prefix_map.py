# Build paths out of the image: the framework's sources use __FILE__ (asserts, logs), which would carry the
# builder's home folder into the release image. Map PlatformIO's folders to short neutral names.
Import("env")  # noqa: F821  (PlatformIO's SCons)
import os

for d, name in ((env.subst("$PROJECT_PACKAGES_DIR"), "pio-packages"), (env.subst("$PROJECT_DIR"), "radr")):  # noqa: F821
    for form in {d, d.replace("\\", "/")}:
        env.Append(CCFLAGS=[f"-fmacro-prefix-map={form}={name}"])  # noqa: F821

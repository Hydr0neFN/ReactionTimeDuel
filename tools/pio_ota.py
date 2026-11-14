"""PlatformIO extra script: adds the target `ota` (pio run -t ota, or "Custom > OTA upload" in VS Code).

It builds the image and sends it with `tools/rtd.py ota <kind>`. The kind comes from the option
`custom_ota_kind` (display, host or sticks) of the environment. The PC must be on the Wi-Fi RTD-Host
and the host in update mode.
"""
import os

Import("env")  # noqa: F821  (SCons)

kind = env.GetProjectOption("custom_ota_kind", "")  # noqa: F821
if kind:
    tool = os.path.normpath(os.path.join(env.subst("$PROJECT_DIR"), "..", "..", "tools", "rtd.py"))  # noqa: F821
    env.AddCustomTarget(  # noqa: F821
        name="ota",
        dependencies="$BUILD_DIR/${PROGNAME}.bin",
        actions='"$PYTHONEXE" "%s" ota %s --file "$SOURCE"' % (tool, kind),
        title="OTA upload",
        description="Send this image over the air (Wi-Fi RTD-Host, host in update mode)",
    )

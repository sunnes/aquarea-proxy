# PlatformIO pre-script: injects secrets.env into the build so no secret lives in tracked files.
#   DEVICE_PASSWORD -> C macro (string literal), and the OTA upload password
#   AQUAREA_HOST    -> OTA upload address
import os
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

path = os.path.join(env.subst("$PROJECT_DIR"), "secrets.env")  # noqa: F821


def load(p):
    values = {}
    with open(p) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            values[key.strip()] = val.strip().strip("\"'")
    return values


if not os.path.exists(path):
    raise SystemExit("secrets.env is missing: copy secrets.env.example to secrets.env and fill it in")

secrets = load(path)
password = secrets.get("DEVICE_PASSWORD", "")
if len(password) < 8 or password == "change-me-long-random":
    raise SystemExit("secrets.env: set DEVICE_PASSWORD (8+ characters, not the example value)")

version_file = os.path.join(env.subst("$PROJECT_DIR"), "VERSION")  # noqa: F821
version = open(version_file).read().strip() if os.path.exists(version_file) else "dev"
env.Append(CPPDEFINES=[("AQUAREA_VERSION", env.StringifyMacro(version))])  # noqa: F821
env.Append(CPPDEFINES=[("DEVICE_PASSWORD", env.StringifyMacro(password))])  # noqa: F821

if env.subst("$UPLOAD_PROTOCOL") == "espota":  # noqa: F821
    host = secrets.get("AQUAREA_HOST", "")
    if not host:
        raise SystemExit("secrets.env: set AQUAREA_HOST for OTA upload")
    env.Replace(UPLOAD_PORT=host)  # noqa: F821
    # The unit being updated may still run an older password than the one being built in (after a
    # password change): AQUAREA_OTA_PASSWORD overrides only the upload login, once.
    upload_password = os.environ.get("AQUAREA_OTA_PASSWORD") or password
    env.Append(UPLOAD_FLAGS=["--auth=" + upload_password])  # noqa: F821

"""Stamp the build with a version the artifact can be traced back to.

The VERSION file at the repository root is the single source of truth, and the
release workflow refuses to publish a tag that disagrees with it. `git describe`
is recorded alongside so a binary built between releases still names its commit.

Exposed to the firmware as MMMC_VERSION / MMMC_GIT_REV / MMMC_BUILD; see
src/version.h for the fallbacks used outside a PlatformIO build.
"""

import subprocess
from pathlib import Path

Import("env")  # noqa: F821 - injected by PlatformIO

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # noqa: F821


def read_version():
    try:
        return (PROJECT_DIR / "VERSION").read_text().strip()
    except OSError:
        return "0.0.0"


def git_rev():
    """Describe the checkout, or fall back when git or the history is absent.

    Shallow clones and source tarballs are both normal here, so a failure is
    reported in the macro rather than raised.
    """
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=PROJECT_DIR,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return out.stdout.strip() if out.returncode == 0 else "unknown"


version = read_version()
rev = git_rev()

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("MMMC_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("MMMC_GIT_REV", env.StringifyMacro(rev)),  # noqa: F821
        ("MMMC_BUILD", env.StringifyMacro("%s+%s" % (version, rev))),  # noqa: F821
    ]
)

# Name the firmware after the version so a downloaded .hex is self-identifying.
# Only the firmware env: renaming the native test binaries confuses `pio test`.
if env.subst("$PIOENV") == "teensy41":  # noqa: F821
    env.Replace(PROGNAME="mmmc-%s" % version)  # noqa: F821

print("MMMC build %s+%s" % (version, rev))

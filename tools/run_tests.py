#!/usr/bin/env python3
"""Build and run the native rtd_core test suite.

Compiles every source of the shared library with the simulator and the tests
into .pio_native/rtd_tests and runs it. Standard library only.
"""

import os
import shutil
import subprocess
import sys


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "shared", "rtd_core", "src")
TEST_DIR = os.path.join(ROOT, "tests")
OUT_DIR = os.path.join(ROOT, ".pio_native")
EXE_NAME = "rtd_tests.exe" if os.name == "nt" else "rtd_tests"

CXX_FLAGS = ["-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-I" + SRC_DIR, "-I" + TEST_DIR]


def find_compiler():
    """Return the compiler path in the documented order, or None."""
    candidates = []
    env = os.environ.get("CXX")
    if env:
        candidates.append(env.split()[0])
    found = shutil.which("g++")
    if found:
        candidates.append(found)
    candidates.append("C:/msys64/mingw64/bin/g++.exe")
    found = shutil.which("clang++")
    if found:
        candidates.append(found)

    for cand in candidates:
        if os.path.isfile(cand):
            return os.path.abspath(cand)
        found = shutil.which(cand)
        if found:
            return os.path.abspath(found)
    return None


def sources(directory, ext):
    return sorted(
        os.path.join(directory, name)
        for name in os.listdir(directory)
        if name.endswith(ext)
    )


def main():
    cxx = find_compiler()
    if not cxx:
        sys.stderr.write(
            "error: no C++ compiler found.\n"
            "Tried the CXX environment variable, g++ on PATH,\n"
            "C:/msys64/mingw64/bin/g++.exe and clang++ on PATH.\n")
        return 2

    # Make the compiler's own DLLs reachable for the child processes.
    cxx_dir = os.path.dirname(cxx)
    env = dict(os.environ)
    env["PATH"] = cxx_dir + os.pathsep + env.get("PATH", "")

    files = sources(SRC_DIR, ".cpp") + sources(TEST_DIR, ".cpp")
    if not files:
        sys.stderr.write("error: no sources found\n")
        return 2

    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)
    exe = os.path.join(OUT_DIR, EXE_NAME)

    cmd = [cxx] + CXX_FLAGS + ["-o", exe] + files
    print("compiler: " + cxx)
    print(" ".join('"%s"' % part if " " in part else part for part in cmd))
    result = subprocess.run(cmd, env=env)
    if result.returncode != 0:
        sys.stderr.write("error: compilation failed\n")
        return result.returncode

    print("running " + EXE_NAME)
    result = subprocess.run([exe], env=env)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())

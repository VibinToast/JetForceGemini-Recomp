#!/usr/bin/env python3
"""Writes the version in version.txt into the build folder's copy of the base project, after the patches:
    python3 tools/set_version.py <version.txt> <build folder>
CMakeLists.txt (project VERSION) and src/main.cpp (version_string, shown by the launcher) get the new number.
The release workflow (.github/workflows/release.yml) updates version.txt on every release."""
import os, re, sys

def stamp(path, pattern, replacement):
    with open(path, "rb") as f:
        data = f.read()
    new, n = re.subn(pattern, replacement, data, count=1)
    if n != 1:
        sys.exit(f"[jfg] ERROR: version not found in {path}")
    if new != data:
        with open(path, "wb") as f:
            f.write(new)

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as f:
        version = f.read().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit(f"[jfg] ERROR: version.txt must hold a version like 1.2.3, not '{version}'")
    work = sys.argv[2]
    v = version.encode()
    stamp(os.path.join(work, "CMakeLists.txt"), rb"(project\(JFGRecomp VERSION )\d+\.\d+\.\d+", rb"\g<1>" + v)
    stamp(os.path.join(work, "src", "main.cpp"), rb'(const std::string version_string = ")\d+\.\d+\.\d+(";)', rb"\g<1>" + v + rb"\g<2>")
    print(f"[jfg] version {version}")

if __name__ == "__main__":
    main()

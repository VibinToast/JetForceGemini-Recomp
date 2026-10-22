#!/usr/bin/env python3
"""Prints one hash of everything the generated game code (RecompiledFuncs, rsp/aspMain.cpp) is made from:
    python3 tools/recomp_inputs_hash.py <file> [<file> ...]
The build scripts pass the ROM, the recompiler configs, the scripts in tools/ and the recompiler executables.
When the hash matches the one saved with the last generated code, the build skips generating it again, so the
generated files keep their timestamps and are not compiled again."""
import glob, hashlib, sys

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    paths = []
    for arg in sys.argv[1:]:
        # cmd.exe does not expand wildcards (tools\*.py), so they are expanded here.
        paths += sorted(glob.glob(arg)) if any(c in arg for c in "*?[") else [arg]
    total = hashlib.sha256()
    for path in sorted(paths):
        h = hashlib.sha256()
        try:
            with open(path, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
        except OSError:
            h.update(b"<missing>")
        # The name too, so a renamed or moved input changes the hash. Path separators differ between the
        # Windows and Linux scripts, so only the file name counts.
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        total.update(name.encode() + b"\0" + h.digest())
    print(total.hexdigest())

if __name__ == "__main__":
    main()

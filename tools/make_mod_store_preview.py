"""Builds bin/mod_store_preview: with JFG_MOD_STORE_PREVIEW=1 the launcher's Get Mods page reads its catalogs and files
from there instead of GitHub, by the same URLs (src/jfg/jfg_mod_store.cpp), to try catalogs before publishing them.

Each collection is laid out as its GitHub releases would be:
    mod_store_preview/<repo>/latest/catalog.json             (releases/latest/download/catalog.json)
    mod_store_preview/<repo>/<mod id>-v<version>/<mod id>.nrm (releases/download/<tag>/<file>)
    mod_store_preview/<repo>/<mod id>-v<version>/thumb.png

Everything in a catalog comes from the built .nrm files (their mod.json and thumb.png), as a publish script would.

    python tools/make_mod_store_preview.py
    python tools/make_mod_store_preview.py --bump jfg_shellback --bump jfg_ammo_ring   # pretend those have updates
"""
import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
MODS_SRC = Path.home() / "Documents/Decomps/N64/JetForceGemini/Mods"  # the official mods' sources (--mods)
OUT = PORT / "bin" / "mod_store_preview"
OWNER = "VibinToast"

# Tags for mods whose files have none in their mod.json yet (built before mod.toml had tags).
TAGS = {
    "jfg_singularity": ["Weapons"], "jfg_concussion_cannon": ["Weapons"], "jfg_plasma_vortex": ["Weapons"],
    "jfg_plasma_ricochet": ["Weapons"], "jfg_better_flamethrower": ["Weapons"], "jfg_better_shocker": ["Weapons"],
    "jfg_weapon_sounds": ["Weapons", "Audio"], "jfg_shellback": ["Enemies"], "jfg_more_enemies": ["Enemies", "Gameplay"],
    "jfg_ammo_ring": ["HUD"], "jfg_health_ring": ["HUD"], "jfg_kill_feed": ["HUD"], "jfg_minimap": ["HUD"],
    "jfg_reticle": ["HUD"], "jfg_blood": ["Visuals"], "jfg_better_floyd": ["Gameplay"], "jfg_dodge": ["Gameplay"],
    "jfg_aim_toggle": ["Controls"], "jfg_better_pickups": ["Gameplay"],
    "jfg_restored_content": ["Content"], "jfg_restored_content_test_hub": ["Levels", "Content"],
}


def bump(version: str) -> str:
    parts = [int(p) for p in version.split(".")] + [0, 0, 0]
    parts = parts[:3]
    parts[2] += 1
    return ".".join(str(p) for p in parts)


def collection(repo: str, name: str, nrm_files, bumped) -> None:
    root = OUT / repo
    shutil.rmtree(root, ignore_errors=True)
    mods = []
    for nrm in sorted(nrm_files):
        with zipfile.ZipFile(nrm) as z:
            manifest = json.loads(z.read("mod.json"))
            thumb = z.read("thumb.png") if "thumb.png" in z.namelist() else None
        mod_id = manifest["id"]
        version = manifest.get("version", "0.0.0")
        if mod_id in bumped:
            version = bump(version)  # the same file, listed as a newer version: shows the Update button
        tag = f"{mod_id}-v{version}"
        release = root / tag
        release.mkdir(parents=True)
        shutil.copyfile(nrm, release / f"{mod_id}.nrm")
        base = f"https://github.com/{OWNER}/{repo}/releases/download/{tag}/"
        entry = {
            "id": mod_id,
            "name": manifest.get("display_name", mod_id),
            "short_description": manifest.get("short_description", ""),
            "description": " ".join(manifest.get("description", "").split()),
            "version": version,
            "authors": manifest.get("authors") or [],
            "tags": manifest.get("tags") or TAGS.get(mod_id, []),  # the mod.toml's tags; TAGS for files built without
            "file": base + f"{mod_id}.nrm",
            "size": nrm.stat().st_size,
            "sha256": hashlib.sha256(nrm.read_bytes()).hexdigest(),
            "minimum_port_version": manifest.get("minimum_recomp_version", ""),
            "dependencies": [d.split(":")[0] for d in (manifest.get("dependencies") or [])],
        }
        if thumb:
            (release / "thumb.png").write_bytes(thumb)
            entry["thumbnail"] = base + "thumb.png"
        mods.append(entry)
    latest = root / "latest"
    latest.mkdir(parents=True)
    catalog = {"collection": repo, "name": name, "owner": OWNER, "mods": mods}
    (latest / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    print(f"{repo}: {len(mods)} mods")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--mods", default=str(MODS_SRC), help="the folder of mod sources (each with build/<id>.nrm)")
    p.add_argument("--bump", action="append", default=[], help="list this mod one version newer than its file")
    a = p.parse_args()
    mods_src = Path(a.mods)
    official = [f for d in sorted(mods_src.glob("jfg_*")) if (d / "mod.toml").is_file()
                for f in [d / "build" / f"{d.name}.nrm"] if f.is_file()]
    # The collection's level packs (built by JFG-Studio into bin/mods).
    official += [f for f in [PORT / "bin" / "mods" / "toast_shellback_levels.nrm"] if f.is_file()]
    restored = [f for f in (PORT / "bin" / "mods").glob("jfg_restored_content*.nrm")]
    OUT.mkdir(parents=True, exist_ok=True)
    collection("JFG-Mods", "Official Mods", official, set(a.bump))
    collection("JFG-Restored-Content", "Restored Content", restored, set(a.bump))


if __name__ == "__main__":
    main()

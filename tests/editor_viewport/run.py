#!/usr/bin/env python3
"""Exercise live-info-panel attachment in the real Godot editor.

Oracle: the panel must enter the editor tree above all four 3D viewports.
The original main-screen lookup fails this check on Godot 4.8.dev7.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MARKER = "PASS: live info panel is attached above all four 3D viewports"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True)
    parser.add_argument("--native-addon", type=Path, required=True,
                        help="Built addon supplying bin/ and terrain.gdextension")
    parser.add_argument("--logs", type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="terrain-editor-", dir=args.logs) as directory:
        scratch = Path(directory)
        project = scratch / "project"
        addon = project / "addons/terrain_3d"
        shutil.copytree(ROOT / "project/addons/terrain_3d", addon,
                        ignore=shutil.ignore_patterns("bin", "*.import"))
        shutil.copytree(args.native_addon / "bin", addon / "bin")
        shutil.copyfile(args.native_addon / "terrain.gdextension", addon / "terrain.gdextension")
        probe = project / "addons/probe"
        probe.mkdir()
        shutil.copyfile(Path(__file__).with_name("probe.gd"), probe / "probe.gd")
        (probe / "plugin.cfg").write_text(
            '[plugin]\nname="Viewport probe"\ndescription="Editor attachment regression"\n'
            'author="Reverie"\nversion="1"\nscript="probe.gd"\n')
        config = ('config_version=5\n[application]\nconfig/name="Terrain editor test"\n'
                  '[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
        (project / "project.godot").write_text(config)
        env = dict(os.environ)
        for key, folder in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                            ("XDG_CACHE_HOME", "cache")):
            env[key] = str((scratch / folder).resolve())
        for name, options in (("prepare", ["--quit-after", "30"]), ("editor", [])):
            if name == "editor":
                (project / "project.godot").write_text(
                    config + '[editor_plugins]\nenabled=PackedStringArray('
                    '"res://addons/terrain_3d/plugin.cfg", "res://addons/probe/plugin.cfg")\n')
            command = [args.godot, "--headless", "--editor", "--path", str(project), *options]
            try:
                result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, timeout=40)
            except subprocess.TimeoutExpired:
                print(f"FAIL: {name} exceeded 40 seconds")
                return 1
            (args.logs / f"{name}.log").write_text(result.stdout)
            errors = [line for line in result.stdout.splitlines()
                      if "ERROR:" in line or line.startswith("FAIL:")]
            if result.returncode != 0 or errors or (name == "editor" and MARKER not in result.stdout):
                print(f"FAIL: {name} exit={result.returncode}")
                print("\n".join(errors) or result.stdout)
                return 1
        print(MARKER)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

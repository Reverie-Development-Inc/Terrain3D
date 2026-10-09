#!/usr/bin/env python3
"""Run native Assets lifecycle controls against a built Terrain3D addon.

Oracle: unsaved/embedded Assets survive tree entry and READY; externally saved
Assets retain the reload/free optimization. REV-1778's script-free scenes must
emit no ERROR lines. A successful Godot exit alone is not a passing result.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
NODE = '''[node name="Root" type="Node3D"]
[node name="Camera3D" type="Camera3D" parent="."]
position = Vector3(0, 10, 10)
current = true
[node name="Terrain3D" type="Terrain3D" parent="."]
'''
MAT = '[sub_resource type="Terrain3DMaterial" id="Mat"]\nworld_background = 0\n'
SCENES = {
    "bare": '[gd_scene format=3]\n' + NODE,
    "directory": '[gd_scene format=3]\n' + NODE + 'data_directory = "res://terrain_data/"\n',
    "material": '[gd_scene load_steps=2 format=3]\n' + MAT + NODE + 'material = SubResource("Mat")\n',
    "material_default": '[gd_scene load_steps=2 format=3]\n[sub_resource type="Terrain3DMaterial" id="Mat"]\n' + NODE + 'material = SubResource("Mat")\n',
    "material_no_free": '[gd_scene load_steps=2 format=3]\n' + MAT + NODE + 'material = SubResource("Mat")\nfree_editor_textures = false\n',
    "material_external_assets": '[gd_scene load_steps=3 format=3]\n[ext_resource type="Terrain3DAssets" path="res://external_assets.tres" id="External"]\n' + MAT + NODE + 'material = SubResource("Mat")\nassets = ExtResource("External")\n',
    "material_embedded_assets": '[gd_scene load_steps=3 format=3]\n' + MAT + '[sub_resource type="Terrain3DAssets" id="Assets"]\n' + NODE + 'material = SubResource("Mat")\nassets = SubResource("Assets")\n',
    "directory_populated": '[gd_scene format=3]\n' + NODE + 'data_directory = "res://populated_data/"\ndebug_level = 1\n',
    "material_info": '[gd_scene load_steps=2 format=3]\n' + MAT + NODE + 'material = SubResource("Mat")\ndebug_level = 3\n',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True)
    parser.add_argument("--addon", type=Path, default=ROOT / "project/addons/terrain_3d")
    parser.add_argument("--logs", type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    failures = []
    with tempfile.TemporaryDirectory(prefix="terrain-assets-", dir=args.logs) as directory:
        project = Path(directory)
        (project / "addons").mkdir()
        (project / "addons/terrain_3d").symlink_to(args.addon.resolve(), target_is_directory=True)
        (project / ".godot").mkdir()
        (project / ".godot/extension_list.cfg").write_text("res://addons/terrain_3d/terrain.gdextension\n")
        (project / "terrain_data").mkdir()
        (project / "populated_data").mkdir()
        (project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Assets lifecycle"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
        (project / "external_assets.tres").write_text('[gd_resource type="Terrain3DAssets" format=3]\n[resource]\n')
        for name, scene in SCENES.items():
            (project / (name + ".tscn")).write_text(scene)
        for script in ("prepare.gd", "lifecycle.gd"):
            shutil.copyfile(Path(__file__).with_name(script), project / script)

        def run(name, options, marker=None):
            command = [args.godot, "--headless", "--path", str(project), *options]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            (args.logs / (name + ".log")).write_text(result.stdout)
            errors = [line for line in result.stdout.splitlines() if "ERROR:" in line or line.startswith("FAIL:")]
            if result.returncode != 0 or errors or (marker and marker not in result.stdout):
                failures.append(name)
                print(f"FAIL {name}: exit={result.returncode}")
                print("\n".join(errors) or result.stdout)
                return False
            print(f"PASS {name}")
            return True

        if run("prepare", ["--script", "res://prepare.gd"], "PREPARED_REGION"):
            for name in SCENES:
                run(name, ["--quit-after", "3", "res://" + name + ".tscn"])
            run("lifecycle", ["--script", "res://lifecycle.gd"], "ASSETS_LIFECYCLE_PASS")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())

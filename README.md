![Terrain3D Logo](/doc/docs/images/terrain3d.jpg)

# Terrain3D
A high performance, editable terrain system for Godot 4.

## Reverie engine pin

The `reverie-64tex-4.8` fork builds against Godot **4.8-dev7**
(`4.8.dev7.official.c971f93e7`). All CI platforms use the committed API
and interface dumps in `gdextension/`. The SCons cache key includes their
content hash. The godot-cpp submodule remains pinned at `60b5a4196de8`.

The dumps came from the [official Linux x86_64 archive](https://godotengine.org/download/archive/4.8-dev7/).
Its SHA512, checked against the release's `SHA512-SUMS.txt`, is:

```text
6444078c4984699069e81f4c9fb83b88119a89d6f4dd39790acf9c2e2b1e4d0fb435a0c101aa48d24a81662d2ff50d648bd3475086d2d2ca37ce90374b97ef38
```

Dump SHA256:

```text
b9b10796e8806e0e83ea4601b3f4c97e07406accedaf598937050b4abf59afa1  extension_api.json
c312ed1dfe6bba973018f0937b38220069467c000ee2aa3f23c34997b56f299a  gdextension_interface.h
```

Rebuild Linux debug/release and Windows x86_64 debug/release in the
importer's `rwi-build:22.04` container (GLIBC 2.35). Pass the same two
dump paths to both Terrain3D and reverie-world-importer:

```sh
scons platform=linux target=template_debug arch=x86_64 -j4 \
  debug_symbols=no use_static_cpp=yes \
  custom_api_file="$PWD/gdextension/extension_api.json" \
  gdextension_dir="$PWD/gdextension"
```

Use `target=template_release` for release. Use `platform=windows` for
MinGW cross-builds. Check every Linux library's imported GLIBC symbols
before distribution. The ceiling is 2.35.

The pre-flip rollback tag is `godot-4.8-dev4-last`. Restore the engine,
native binaries, Rust bindings, and export templates together on rollback.


## Features
* Written in C++ as a GDExtension addon, which works with official builds of Godot Engine
* [Can be accessed](https://terrain3d.readthedocs.io/en/stable/docs/programming_languages.html) by GDScript, C#, and any language Godot supports
* Terrains as small as 64x64m up to 65.5x65.5km (4295km^2) in non-contiguous and variable sized regions
* Up to 32 textures
* Up to 10 levels of detail for the terrain mesh
* Foliage instancing, with up to 10 levels of detail, and a shadow impostor
* Sculpting, holes, texture painting, texture detiling, painting colors and wetness
* Imports heightmaps from [HTerrain](https://github.com/Zylann/godot_heightmap_plugin/), Gaea, World Creator, World Machine, Unity, Unreal and any tool that can export a heightmap. See [heightmaps](https://terrain3d.readthedocs.io/en/stable/docs/heightmaps.html)


## Games Using Terrain3D

Please see the [featured games using Terrain3D](https://terrain3d.readthedocs.io/en/latest/docs/games.html) for examples of what it can do.


## Getting Started

1. Read the [Introduction](https://terrain3d.readthedocs.io/en/stable/docs/introduction.html) to understand how this terrain system works.

2. Read the [Installation & Upgrade](https://terrain3d.readthedocs.io/en/stable/docs/installation.html) instructions.

3. Watch the [tutorial videos](https://terrain3d.readthedocs.io/en/stable/docs/tutorial_videos.html) and read through the documentation.

4. For support, read [Getting Help](https://terrain3d.readthedocs.io/en/stable/docs/getting_help.html) and join our [Discord server](https://tokisan.com/discord).


## Credit
Developed for the Godot community by:

|||
|--|--|
| **Cory Petkovsek, Tokisan Games** | [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/twitter.png?raw=true" width="24"/>](https://twitter.com/TokisanGames) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/github.png?raw=true" width="24"/>](https://github.com/TokisanGames) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/www.png?raw=true" width="24"/>](https://tokisan.com/) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/discord.png?raw=true" width="24"/>](https://tokisan.com/discord) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/youtube.png?raw=true" width="24"/>](https://www.youtube.com/@TokisanGames)|
| **Roope Palmroos, Outobugi Games** | [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/twitter.png?raw=true" width="24"/>](https://twitter.com/outobugi) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/github.png?raw=true" width="24"/>](https://github.com/outobugi) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/www.png?raw=true" width="24"/>](https://outobugi.com/) [<img src="https://github.com/dmhendricks/signature-social-icons/blob/master/icons/round-flat-filled/35px/youtube.png?raw=true" width="24"/>](https://www.youtube.com/@outobugi)|

And the contribution team in [AUTHORS.md](https://terrain3d.readthedocs.io/en/stable/docs/authors.html) and on the right of the github page.


## Contributing

Please see [Contributing](https://terrain3d.readthedocs.io/en/latest/docs/contributing.html) if you would like to help make Terrain3D the best terrain system for Godot.


## License

This addon has been released under the [MIT License](https://github.com/TokisanGames/Terrain3D/blob/main/LICENSE.txt).

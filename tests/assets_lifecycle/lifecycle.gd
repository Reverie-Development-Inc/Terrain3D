extends SceneTree

var failures: int = 0


func _initialize() -> void:
	call_deferred("_run")


func _check(condition: bool, message: String) -> void:
	if not condition:
		print("FAIL: " + message)
		failures += 1


func _assets() -> Resource:
	var assets: Resource = ClassDB.instantiate("Terrain3DAssets")
	var texture: Resource = ClassDB.instantiate("Terrain3DTextureAsset")
	texture.set("name", "lifecycle-canary")
	texture.set("albedo_color", Color(0.2, 0.4, 0.6, 1.0))
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color.RED)
	texture.set("albedo_texture", ImageTexture.create_from_image(image))
	image = Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.5, 0.5, 1.0, 1.0))
	texture.set("normal_texture", ImageTexture.create_from_image(image))
	assets.call("set_texture_asset", 0, texture)
	return assets


func _retained(terrain: Node, expected: Resource, label: String) -> void:
	var actual: Resource = terrain.get("assets")
	_check(actual == expected, label + ": Assets identity changed")
	var texture: Resource = actual.call("get_texture_asset", 0)
	_check(texture != null, label + ": texture was discarded")
	if texture:
		_check(texture.get("name") == "lifecycle-canary", label + ": texture contents changed")
		_check(texture.get("albedo_color") == Color(0.2, 0.4, 0.6, 1.0), label + ": texture color changed")


func _renderable(terrain: Node, label: String) -> void:
	var assets: Resource = terrain.get("assets")
	var albedo: RID = assets.call("get_albedo_array_rid")
	var normal: RID = assets.call("get_normal_array_rid")
	_check(albedo.is_valid(), label + ": generated albedo array was discarded")
	_check(normal.is_valid(), label + ": generated normal array was discarded")
	_check(not terrain.get("material").get("show_checkered"), label + ": material fell back to checkerboard")


func _run() -> void:
	var camera := Camera3D.new()
	root.add_child(camera)
	camera.current = true
	for kind: String in ["memory", "embedded", "external", "external_named"]:
		var assets: Resource = _assets()
		var terrain: Node3D = ClassDB.instantiate("Terrain3D")
		var material: Resource = ClassDB.instantiate("Terrain3DMaterial")
		material.set("world_background", 0)
		terrain.set("material", material)
		terrain.set("assets", assets)
		if kind == "embedded":
			var packed := PackedScene.new()
			_check(packed.pack(terrain) == OK, "pack embedded scene")
			_check(ResourceSaver.save(packed, "res://embedded.tscn") == OK, "save embedded scene")
			terrain.free()
			terrain = load("res://embedded.tscn").instantiate()
			assets = terrain.get("assets")
		elif kind.begins_with("external"):
			var path: String = "res://Terrain3DAssets.tres" if kind == "external_named" else "res://saved.tres"
			_check(ResourceSaver.save(assets, path) == OK, "save external Assets")
			assets.take_over_path(path)
		root.add_child(terrain)
		_renderable(terrain, kind + " READY")
		if kind.begins_with("external"):
			_check(terrain.get("assets") != assets, kind + ": external Assets were not reloaded")
			_check(terrain.get("assets").call("get_texture_count") == 0, kind + ": READY did not clear textures")
			_check(assets.call("get_texture_count") == 1, kind + ": shared source Assets were cleared")
		else:
			_retained(terrain, assets, kind + " READY")
		root.remove_child(terrain)
		root.add_child(terrain)
		_renderable(terrain, kind + " re-entry")
		if kind.begins_with("external"):
			var reloaded: Resource = terrain.get("assets")
			var texture: Resource = reloaded.call("get_texture_asset", 0)
			_check(texture != null, kind + ": tree re-entry did not restore saved texture")
			if texture:
				_check(texture.get("name") == "lifecycle-canary", kind + ": restored wrong texture")
		else:
			_retained(terrain, assets, kind + " re-entry")
		terrain.free()
	camera.free()
	if failures == 0:
		print("ASSETS_LIFECYCLE_PASS")
	call_deferred("_finish")


func _finish() -> void:
	await process_frame
	quit(1 if failures else 0)

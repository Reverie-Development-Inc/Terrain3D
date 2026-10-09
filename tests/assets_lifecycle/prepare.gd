extends SceneTree


func _initialize() -> void:
	if not ClassDB.class_exists("Terrain3D"):
		push_error("Terrain3D extension did not register")
		quit(1)
		return
	var region: Resource = ClassDB.instantiate("Terrain3DRegion")
	region.set("region_size", 1024)
	region.call("sanitize_maps")
	region.set("location", Vector2i.ZERO)
	var result: int = region.call("save", "res://populated_data/terrain3d_00_00.res")
	if result != OK:
		push_error("Could not save populated-directory region: %s" % result)
		quit(1)
		return
	print("PREPARED_REGION")
	quit(0)

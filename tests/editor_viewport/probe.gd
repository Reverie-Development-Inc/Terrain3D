@tool
extends EditorPlugin


func _enter_tree() -> void:
	call_deferred("_run")


func _run() -> void:
	var plugin := _find_plugin(get_tree().root)
	if plugin == null:
		_fail("Terrain3D editor plugin did not initialize")
		return
	var panel: Control = plugin.ui.live_info_panel
	if panel == null or not panel.is_inside_tree() or not panel.is_node_ready():
		_fail("live info panel is not attached to the editor")
		return
	for index in range(4):
		var viewport := EditorInterface.get_editor_viewport_3d(index)
		if viewport == null or not panel.get_parent().is_ancestor_of(viewport):
			_fail("live info panel does not cover 3D viewport %d" % index)
			return
	print("PASS: live info panel is attached above all four 3D viewports")
	get_tree().quit(0)


func _find_plugin(node: Node) -> EditorPlugin:
	var script := node.get_script() as Script
	if script != null and script.resource_path == "res://addons/terrain_3d/src/editor_plugin.gd":
		return node as EditorPlugin
	for child in node.get_children(true):
		var found := _find_plugin(child)
		if found != null:
			return found
	return null


func _fail(message: String) -> void:
	printerr("FAIL: " + message)
	get_tree().quit(1)

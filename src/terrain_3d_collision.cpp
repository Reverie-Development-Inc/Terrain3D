// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/height_map_shape3d.hpp>
#include <godot_cpp/classes/os.hpp>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <godot_cpp/classes/scene_tree.hpp>


#include "constants.h"
#include "logger.h"
#include "terrain_3d.h"
#include "terrain_3d_collision.h"
#include "terrain_3d_data.h"
#include "terrain_3d_util.h"

///////////////////////////
// Private Functions
///////////////////////////

bool Terrain3DCollision::_incremental_runtime() const {
	return _job_budget > 0 && _mode == DYNAMIC_GAME && !IS_EDITOR;
}

bool Terrain3DCollision::_incremental_owned() const {
	return _incremental_runtime() || _incremental_cleanup;
}

int Terrain3DCollision::_shape_count() const {
	if (_incremental_owned()) {
		return _allocated_slots;
	}
	return is_editor_mode() ? int(_shapes.size()) : (_static_body_rid.is_valid() ? PS->body_get_shape_count(_static_body_rid) : 0);
}

void Terrain3DCollision::_invalidate_epoch() {
	if (++_epoch == 0) {
		_epoch = 1;
	}
	for (ShapeSlot &slot : _slots) {
		slot.epoch = 0;
	}
}

void Terrain3DCollision::_record_job(const String &p_operation, uint64_t p_usec, int p_page_samples) {
	_completed_jobs++;
	_max_job_usec = MAX(_max_job_usec, p_usec);
	_max_page_samples = MAX(_max_page_samples, p_page_samples);
	_last_operation = p_operation;
	Dictionary stats = _operation_stats.get(p_operation, Dictionary());
	stats["count"] = int(stats.get("count", 0)) + 1;
	stats["max_usec"] = MAX(uint64_t(stats.get("max_usec", 0)), p_usec);
	if (p_page_samples > 0) {
		stats["max_page_samples"] = MAX(int(stats.get("max_page_samples", 0)), p_page_samples);
	}
	_operation_stats[p_operation] = stats;
}

Dictionary Terrain3DCollision::_job_result(const String &p_operation, uint64_t p_usec) const {
	Dictionary result;
	result["operation"] = p_operation;
	result["pending"] = !_jobs.empty();
	result["usec"] = p_usec;
	return result;
}

void Terrain3DCollision::_queue(JobKind p_kind, int p_slot, const Vector2i &p_position) {
	Job job;
	job.kind = p_kind;
	job.slot = p_slot;
	job.position = p_position;
	job.epoch = _epoch;
	_jobs.push_back(job);
}

void Terrain3DCollision::_push_front(const Job &p_job) {
	_jobs.push_front(p_job);
}

void Terrain3DCollision::_clear_pending_jobs() {
	_jobs.clear();
	_pending_page.clear();
	_pending_page_slot = -1;
	_pending_page_epoch = 0;
}

void Terrain3DCollision::_queue_missing_shapes() {
	for (int i = 0; i < _required_slots; i++) {
		if (!_slots[i].rid.is_valid()) {
			_queue(JobKind::ALLOCATE_SHAPE, i);
		}
	}
}

void Terrain3DCollision::_queue_reconciliation() {
	for (int i = 0; i < _required_slots; i++) {
		_queue(JobKind::RECONCILE, i);
	}
}

void Terrain3DCollision::_queue_disable_all() {
	for (int i = 0; i < int(_slots.size()); i++) {
		if (_slots[i].rid.is_valid()) {
			_queue(JobKind::DISABLE, i);
		}
	}
}

void Terrain3DCollision::_queue_retirement() {
	_queue_disable_all();
	for (int i = 0; i < int(_slots.size()); i++) {
		if (_slots[i].rid.is_valid()) {
			_queue(JobKind::FREE_SHAPE, i);
		}
	}
	if (_static_body_rid.is_valid()) {
		_queue(JobKind::FREE_BODY);
	}
}

void Terrain3DCollision::_configure_runtime() {
	int grid_width = int_ceil_pow2(_radius * 2 / _shape_size, 4);
	_grid_width = grid_width;
	_required_slots = grid_width * grid_width;
	if (int(_slots.size()) < _required_slots) {
		_slots.resize(_required_slots);
	}
}

void Terrain3DCollision::_mark_slot_unused(int p_slot) {
	if (p_slot < 0 || p_slot >= int(_slots.size())) {
		return;
	}
	_slots[p_slot].position = V2I_MAX;
	_slots[p_slot].epoch = 0;
	_slots[p_slot].installed = false;
	_slots[p_slot].enabled = false;
}

void Terrain3DCollision::_set_slot_disabled(int p_slot, bool p_disabled) {
	if (p_slot < 0 || p_slot >= int(_slots.size()) || !_slots[p_slot].rid.is_valid()) {
		return;
	}
	PS->body_set_shape_disabled(_static_body_rid, p_slot, p_disabled);
	_slots[p_slot].enabled = !p_disabled;
}

void Terrain3DCollision::_set_slot_transform(int p_slot, const Transform3D &p_transform) {
	if (p_slot < 0 || p_slot >= int(_slots.size()) || !_slots[p_slot].rid.is_valid()) {
		return;
	}
	PS->body_set_shape_transform(_static_body_rid, p_slot, p_transform);
}


void Terrain3DCollision::_set_slot_data(int p_slot, const Dictionary &p_data) {
	if (p_slot < 0 || p_slot >= int(_slots.size()) || !_slots[p_slot].rid.is_valid()) {
		return;
	}
	PS->shape_set_data(_slots[p_slot].rid, p_data);
}

Dictionary Terrain3DCollision::_sample_shape_page(const Vector2i &p_position, int p_size, int &r_samples) {
	r_samples = 0;
	IS_DATA_INIT_MESG("Terrain not initialized", Dictionary());
	const Terrain3DData *data = _terrain->get_data();
	Vector2i region_loc = V2I_DIVIDE_FLOOR(p_position, _terrain->get_region_size());
	const Terrain3DRegion *region = data->get_region_ptr(region_loc);
	if (!region || region->is_deleted()) {
		return Dictionary();
	}
	int hshape_size = p_size + 1;
	PackedRealArray map_data;
	map_data.resize(hshape_size * hshape_size);
	std::vector<uint8_t> finite(hshape_size * hshape_size, 0);
	real_t min_height = FLT_MAX;
	real_t max_height = -FLT_MAX;
	for (int z = 0; z < hshape_size; z++) {
		for (int x = 0; x < hshape_size; x++) {
			r_samples++;
			int index = hshape_size - 1 - z + x * hshape_size;
			real_t height = data->get_modified_height(p_position + Vector2i(x, z));
			if (std::isfinite(height)) {
				finite[z * hshape_size + x] = 1;
				min_height = MIN(min_height, height);
				max_height = MAX(max_height, height);
			} else {
				height = NAN;
			}
			map_data[index] = height;
		}
	}
	bool has_surface = false;
	for (int z = 0; z < p_size && !has_surface; z++) {
		for (int x = 0; x < p_size && !has_surface; x++) {
			int i = z * hshape_size + x;
			has_surface = finite[i] + finite[i + 1] + finite[i + hshape_size] + finite[i + hshape_size + 1] >= 3;
		}
	}
	if (!has_surface) {
		return Dictionary();
	}
	Dictionary shape_data;
	shape_data["width"] = hshape_size;
	shape_data["depth"] = hshape_size;
	shape_data["heights"] = map_data;
	shape_data["xform"] = Transform3D(Basis(V3_UP, Math_PI * .5), v2iv3(p_position + V2I(p_size / 2)));
	shape_data["min_height"] = min_height;
	shape_data["max_height"] = max_height;
	return shape_data;
}

bool Terrain3DCollision::_step_one() {
	if (_jobs.empty()) {
		return false;
	}
	uint64_t started = Time::get_singleton()->get_ticks_usec();
	Job job = _jobs.front();
	_jobs.pop_front();
	String operation = "idle";
	int page_samples = 0;
	auto stale = [&]() { return job.epoch != _epoch || job.slot < 0 || job.slot >= int(_slots.size()); };
	switch (job.kind) {
		case JobKind::ALLOCATE_BODY: {
			operation = "allocate_body";
			if (_terrain && !_static_body_rid.is_valid()) {
				_static_body_rid = PS->body_create();
				PS->body_set_mode(_static_body_rid, PhysicsServer3D::BODY_MODE_STATIC);
				PS->body_set_collision_mask(_static_body_rid, _mask);
				PS->body_set_collision_layer(_static_body_rid, _layer);
				PS->body_set_collision_priority(_static_body_rid, _priority);
				PS->body_attach_object_instance_id(_static_body_rid, _terrain->get_instance_id());
			}
			if (_terrain && _terrain->is_inside_world() && _terrain->get_world_3d().is_valid() && _static_body_rid.is_valid()) {
				PS->body_set_space(_static_body_rid, _terrain->get_world_3d()->get_space());
				_body_attached = true;
				_reload_physics_material();
			}
		} break;
		case JobKind::ALLOCATE_SHAPE: {
			operation = "allocate_shape";
			if (!stale() && !_slots[job.slot].rid.is_valid() && _static_body_rid.is_valid()) {
				RID shape_rid = PS->heightmap_shape_create();
				_slots[job.slot].rid = shape_rid;
				PS->body_add_shape(_static_body_rid, shape_rid, Transform3D(Basis(), V3_MAX), true);
				_allocated_slots++;
				_mark_slot_unused(job.slot);
			}
		} break;
		case JobKind::RECONCILE: {
			operation = "reconcile";
			if (_runtime_state == RuntimeState::READY && _terrain && !stale() && _slots[job.slot].rid.is_valid()) {
				Vector2i snapped_pos = _snap_to_grid(_terrain->get_collision_target_position() / _terrain->get_vertex_spacing());
				Vector2i grid_offset = -V2I(_grid_width / 2);
				Vector2i grid_corner = snapped_pos + grid_offset * _shape_size;
				Vector2i shape_offset = V2I(_shape_size / 2);
				bool reusable = _slots[job.slot].installed && _slots[job.slot].enabled && _slots[job.slot].epoch == _epoch;
				if (reusable) {
					Vector2i grid_loc = (_slots[job.slot].position - grid_corner) / _shape_size;
					int idx = grid_loc.y * _grid_width + grid_loc.x;
					Vector2i center = _slots[job.slot].position + shape_offset;
					reusable = idx == job.slot && center.distance_squared_to(snapped_pos) <= real_t(_radius * _radius);
				}
				if (!reusable) {
					_set_slot_disabled(job.slot, true);
					_mark_slot_unused(job.slot);
					Vector2i grid_loc(job.slot % _grid_width, job.slot / _grid_width);
					Vector2i shape_pos = grid_corner + grid_loc * _shape_size;
					Vector2i center = shape_pos + shape_offset;
					if (center.distance_squared_to(snapped_pos) <= real_t(_radius * _radius)) {
						_queue(JobKind::SAMPLE, job.slot, shape_pos);
					}
				}
			}
		} break;
		case JobKind::SAMPLE: {
			operation = "sample";
			if (_runtime_state == RuntimeState::READY && _terrain && !stale()) {
				_pending_page = _sample_shape_page(job.position, _shape_size, page_samples);
				if (!_pending_page.is_empty() && job.epoch == _epoch) {
					_pending_page_slot = job.slot;
					_pending_page_epoch = job.epoch;
					_pending_page_transform = _pending_page["xform"];
					_pending_page_transform.scale(Vector3(_terrain->get_vertex_spacing(), 1.f, _terrain->get_vertex_spacing()));
					job.kind = JobKind::UPLOAD;
					_push_front(job);
				} else {
					_pending_page.clear();
				}
			}
		} break;
		case JobKind::UPLOAD: {
			operation = "upload";
			bool page_matches = _pending_page_slot == job.slot && _pending_page_epoch == job.epoch && !_pending_page.is_empty();
			if (!stale() && page_matches && _slots[job.slot].rid.is_valid()) {
				_set_slot_transform(job.slot, _pending_page_transform);
				_set_slot_data(job.slot, _pending_page);
				_pending_page.clear();
				_pending_page_slot = -1;
				_pending_page_epoch = 0;
				_slots[job.slot].position = job.position;
				_slots[job.slot].installed = true;
				_slots[job.slot].epoch = job.epoch;
				job.kind = JobKind::ACTIVATE;
				_push_front(job);
			}
		} break;
		case JobKind::ACTIVATE: {
			operation = "activate";
			if (!stale() && _slots[job.slot].installed && _slots[job.slot].epoch == job.epoch && _runtime_state == RuntimeState::READY) {
				_set_slot_disabled(job.slot, false);
			}
		} break;
		case JobKind::DISABLE: {
			operation = "disable";
			if (job.slot >= 0 && job.slot < int(_slots.size()) && _slots[job.slot].rid.is_valid()) {
				_set_slot_disabled(job.slot, true);
				_slots[job.slot].enabled = false;
			}
		} break;
		case JobKind::FREE_SHAPE: {
			operation = "free_shape";
			if (job.slot >= 0 && job.slot < int(_slots.size()) && _slots[job.slot].rid.is_valid() && _static_body_rid.is_valid()) {
				int body_index = -1;
				int body_count = PS->body_get_shape_count(_static_body_rid);
				for (int i = 0; i < body_count; i++) {
					if (PS->body_get_shape(_static_body_rid, i) == _slots[job.slot].rid) {
						body_index = i;
						break;
					}
				}
				if (body_index >= 0) {
					PS->body_remove_shape(_static_body_rid, body_index);
				}
				PS->free_rid(_slots[job.slot].rid);
				_slots[job.slot].rid = RID();
				_mark_slot_unused(job.slot);
				_allocated_slots = MAX(0, _allocated_slots - 1);
			}
		} break;
		case JobKind::FREE_BODY: {
			operation = "free_body";
			if (_static_body_rid.is_valid() && _allocated_slots == 0 && PS->body_get_shape_count(_static_body_rid) == 0) {
				PS->free_rid(_static_body_rid);
				_static_body_rid = RID();
				_body_attached = false;
				_slots.clear();
				_required_slots = 0;
				_runtime_state = RuntimeState::LEGACY;
				_incremental_cleanup = false;
				_initialized = false;
			} else if (_allocated_slots > 0) {
				_queue(JobKind::FREE_BODY);
			}
		} break;
		default:
			operation = "idle";
			break;
	}
	_record_job(operation, Time::get_singleton()->get_ticks_usec() - started, page_samples);
	return true;
}

void Terrain3DCollision::_run_budget() {
	for (int i = 0; i < _job_budget && _automatic_allowed; i++) {
		if (!_step_one()) {
			break;
		}
	}
}

void Terrain3DCollision::_detach_body() {
	if (_static_body_rid.is_valid()) {
		PS->body_set_space(_static_body_rid, RID());
	}
	_body_attached = false;
}

void Terrain3DCollision::_schedule_incremental_cleanup() {
	if (_incremental_cleanup || _jobs.empty() || !_cleanup_tree || !_cleanup_tree->get_root()) {
		return;
	}
	_incremental_cleanup = true;
	_cleanup_tree->connect("process_frame", callable_mp(this, &Terrain3DCollision::_cleanup_step), Object::CONNECT_ONE_SHOT);
}

void Terrain3DCollision::_cleanup_step() {
	_incremental_cleanup = false;
	if (_terrain || _jobs.empty()) {
		return;
	}
	_step_one();
	if (!_jobs.empty() && _cleanup_tree && _cleanup_tree->get_root()) {
		_schedule_incremental_cleanup();
	} else if (_jobs.empty()) {
		memdelete(this);
	}
}

// Calculates shape data from top left position. Assumes descaled and snapped.
Dictionary Terrain3DCollision::_get_shape_data(const Vector2i &p_position, const int p_size) {
	IS_DATA_INIT_MESG("Terrain not initialized", Dictionary());
	const Terrain3DData *data = _terrain->get_data();

	// Get region_loc of top left corner of descaled and grid snapped collision shape position
	Vector2i region_loc = V2I_DIVIDE_FLOOR(p_position, _terrain->get_region_size());
	const Terrain3DRegion *region = data->get_region_ptr(region_loc);
	if (!region || region->is_deleted()) {
		LOG(EXTREME, "Region not found at: ", region_loc, ". Returning blank");
		return Dictionary();
	}

	int hshape_size = p_size + 1; // Calculate last vertex at end
	PackedRealArray map_data = PackedRealArray();
	map_data.resize(hshape_size * hshape_size);
	real_t min_height = FLT_MAX;
	real_t max_height = -FLT_MAX;
	std::vector<uint8_t> finite(hshape_size * hshape_size, 0); // z * hshape_size + x

	for (int z = 0; z < hshape_size; z++) {
		for (int x = 0; x < hshape_size; x++) {
			// Choose array indexing to match triangulation of heightmapshape with the mesh
			// https://stackoverflow.com/questions/16684856/rotating-a-2d-pixel-array-by-90-degrees
			// Normal array index rotated Y=0 - shape rotation Y=0 (xform below)
			// int index = z * hshape_size + x;
			// Array Index Rotated Y=-90 - must rotate shape Y=+90 (xform below)
			int index = hshape_size - 1 - z + x * hshape_size;
			real_t height = data->get_modified_height(p_position + Vector2i(x, z));
			// A non-finite height has no surface, so it becomes a hole like NaN and stays inside the bounds.
			if (std::isfinite(height)) {
				finite[z * hshape_size + x] = 1;
				min_height = MIN(min_height, height);
				max_height = MAX(max_height, height);
			} else {
				height = NAN;
			}
			map_data[index] = height;
		}
	}
	// A triangle needs three finite corners, so a cell with fewer has no surface under either diagonal.
	// Without such a cell the shape has nothing to collide with, and its bounds may be FLT_MAX/-FLT_MAX.
	bool has_surface = false;
	for (int z = 0; z < p_size && !has_surface; z++) {
		for (int x = 0; x < p_size && !has_surface; x++) {
			int i = z * hshape_size + x;
			has_surface = finite[i] + finite[i + 1] + finite[i + hshape_size] + finite[i + hshape_size + 1] >= 3;
		}
	}
	if (!has_surface) {
		LOG(EXTREME, "No cell with a surface at: ", p_position, ". Returning blank");
		return Dictionary();
	}

	// Non rotated shape for normal array index above
	//Transform3D xform = Transform3D(Basis(), global_pos);
	// Rotated shape Y=90 for -90 rotated array index
	Transform3D xform = Transform3D(Basis(V3_UP, Math_PI * .5), v2iv3(p_position + V2I(p_size / 2)));
	Dictionary shape_data;
	shape_data["width"] = hshape_size;
	shape_data["depth"] = hshape_size;
	shape_data["heights"] = map_data;
	shape_data["xform"] = xform;
	shape_data["min_height"] = min_height;
	shape_data["max_height"] = max_height;
	return shape_data;
}

void Terrain3DCollision::_shape_set_disabled(const int p_shape_id, const bool p_disabled) {
	if (is_editor_mode()) {
		CollisionShape3D *shape = _shapes[p_shape_id];
		shape->set_disabled(p_disabled);
		shape->set_visible(!p_disabled);
	} else {
		PS->body_set_shape_disabled(_static_body_rid, p_shape_id, p_disabled);
	}
}

void Terrain3DCollision::_shape_set_transform(const int p_shape_id, const Transform3D &p_xform) {
	if (is_editor_mode()) {
		CollisionShape3D *shape = _shapes[p_shape_id];
		shape->set_transform(p_xform);
	} else {
		PS->body_set_shape_transform(_static_body_rid, p_shape_id, p_xform);
	}
}

Vector3 Terrain3DCollision::_shape_get_position(const int p_shape_id) const {
	if (is_editor_mode()) {
		return _shapes[p_shape_id]->get_global_position();
	} else {
		return PS->body_get_shape_transform(_static_body_rid, p_shape_id).origin;
	}
}

void Terrain3DCollision::_shape_set_data(const int p_shape_id, const Dictionary &p_dict) {
	if (is_editor_mode()) {
		CollisionShape3D *shape = _shapes[p_shape_id];
		Ref<HeightMapShape3D> hshape = shape->get_shape();
		hshape->set_map_data(p_dict["heights"]);
	} else {
		RID shape_rid = PS->body_get_shape(_static_body_rid, p_shape_id);
		PS->shape_set_data(shape_rid, p_dict);
	}
}

void Terrain3DCollision::_reload_physics_material() {
	if (is_editor_mode()) {
		if (_static_body) {
			_static_body->set_physics_material_override(_physics_material);
		}
	} else {
		if (_static_body_rid.is_valid()) {
			if (_physics_material.is_null()) {
				PS->body_set_param(_static_body_rid, PhysicsServer3D::BODY_PARAM_BOUNCE, 0.f);
				PS->body_set_param(_static_body_rid, PhysicsServer3D::BODY_PARAM_FRICTION, 1.f);
			} else {
				real_t computed_bounce = _physics_material->get_bounce() * (_physics_material->is_absorbent() ? -1.f : 1.f);
				real_t computed_friction = _physics_material->get_friction() * (_physics_material->is_rough() ? -1.f : 1.f);
				PS->body_set_param(_static_body_rid, PhysicsServer3D::BODY_PARAM_BOUNCE, computed_bounce);
				PS->body_set_param(_static_body_rid, PhysicsServer3D::BODY_PARAM_FRICTION, computed_friction);
			}
		}
	}
	if (_physics_material.is_valid()) {
		LOG(DEBUG, "Setting PhysicsMaterial bounce: ", _physics_material->get_bounce(), ", friction: ", _physics_material->get_friction());
	}
}

///////////////////////////
// Public Functions
///////////////////////////

void Terrain3DCollision::initialize(Terrain3D *p_terrain) {
	if (p_terrain) {
		_terrain = p_terrain;
	} else {
		return;
	}
	if (!IS_EDITOR && is_editor_mode()) {
		LOG(WARN, "Change collision mode to a non-editor mode for releases");
	}
	build();
}

void Terrain3DCollision::build() {
	IS_DATA_INIT(VOID);
	if (_incremental_runtime()) {
		if (!_terrain->is_inside_world()) {
			LOG(ERROR, "Terrain isn't inside world. Returning.");
			return;
		}
		_configure_runtime();
		_initialized = true;
		if (_runtime_state != RuntimeState::PAUSED) {
			_runtime_state = RuntimeState::READY;
			_automatic_allowed = true;
		}
		_invalidate_epoch();
		_clear_pending_jobs();
		if (!_static_body_rid.is_valid() || !_body_attached) {
			_queue(JobKind::ALLOCATE_BODY);
		}
		_queue_missing_shapes();
		if (_runtime_state == RuntimeState::READY) {
			_queue_reconciliation();
		}
		return;
	}
	if (!_terrain->is_inside_world()) {
		LOG(ERROR, "Terrain isn't inside world. Returning.");
		return;
	}

	// Clear collision as the user might change modes in the editor
	destroy();

	// Build only in applicable modes
	if (!is_enabled() || (IS_EDITOR && !is_editor_mode())) {
		return;
	}

	// Create StaticBody3D
	if (is_editor_mode()) {
		LOG(INFO, "Building editor collision");
		_static_body = memnew(StaticBody3D);
		_static_body->set_name("StaticBody3D");
		_static_body->set_as_top_level(true);
		_terrain->add_child(_static_body, true);
		_static_body->set_owner(_terrain);
		_static_body->set_collision_mask(_mask);
		_static_body->set_collision_layer(_layer);
		_static_body->set_collision_priority(_priority);
	} else {
		LOG(INFO, "Building collision with Physics Server");
		_static_body_rid = PS->body_create();
		PS->body_set_mode(_static_body_rid, PhysicsServer3D::BODY_MODE_STATIC);
		PS->body_set_space(_static_body_rid, _terrain->get_world_3d()->get_space());
		PS->body_attach_object_instance_id(_static_body_rid, _terrain->get_instance_id());
		PS->body_set_collision_mask(_static_body_rid, _mask);
		PS->body_set_collision_layer(_static_body_rid, _layer);
		PS->body_set_collision_priority(_static_body_rid, _priority);
	}
	_reload_physics_material();

	// Create CollisionShape3Ds
	int shape_count;
	int hshape_size;
	if (is_dynamic_mode()) {
		int grid_width = _radius * 2 / _shape_size;
		grid_width = int_ceil_pow2(grid_width, 4);
		shape_count = grid_width * grid_width;
		hshape_size = _shape_size + 1;
		LOG(DEBUG, "Grid width: ", grid_width);
	} else {
		shape_count = _terrain->get_data()->get_region_count();
		hshape_size = _terrain->get_region_size() + 1;
	}
	// Preallocate memory for push_back()
	if (is_editor_mode()) {
		_shapes.reserve(shape_count);
	}
	LOG(DEBUG, "Shape count: ", shape_count);
	LOG(DEBUG, "Shape size: ", _shape_size, ", hshape_size: ", hshape_size);
	Transform3D xform(Basis(), V3_MAX);
	for (int i = 0; i < shape_count; i++) {
		if (is_editor_mode()) {
			CollisionShape3D *col_shape = memnew(CollisionShape3D);
			_shapes.push_back(col_shape);
			col_shape->set_name("CollisionShape3D");
			col_shape->set_disabled(true);
			col_shape->set_visible(true);
			col_shape->set_enable_debug_fill(false);
			Ref<HeightMapShape3D> hshape;
			hshape.instantiate();
			hshape->set_map_width(hshape_size);
			hshape->set_map_depth(hshape_size);
			col_shape->set_shape(hshape);
			_static_body->add_child(col_shape, true);
			col_shape->set_owner(_static_body);
			col_shape->set_transform(xform);
		} else {
			RID shape_rid = PS->heightmap_shape_create();
			PS->body_add_shape(_static_body_rid, shape_rid, xform, true);
			LOG(DEBUG, "Adding shape: ", i, ", rid: ", shape_rid.get_id(), " pos: ", _shape_get_position(i));
		}
	}

	_initialized = true;
	update();
}

void Terrain3DCollision::reset_target_position() {
	_last_snapped_pos = V2I_MAX;
	if (_incremental_runtime() && _runtime_state == RuntimeState::READY) {
		_invalidate_epoch();
		_clear_pending_jobs();
		_queue_reconciliation();
	}
}

void Terrain3DCollision::update(const Vector2i &p_region_loc, const bool p_rebuild) {
	if (_incremental_runtime()) {
		if (_runtime_state != RuntimeState::READY || !_initialized) {
			return;
		}
		Vector2i snapped_pos = _snap_to_grid(_terrain->get_collision_target_position() / _terrain->get_vertex_spacing());
		bool target_moved = (_last_snapped_pos - snapped_pos).length_squared() != 0;
		bool missing_shapes = _allocated_slots < _required_slots;
		if (p_rebuild || target_moved) {
			if (target_moved) {
				_last_snapped_pos = snapped_pos;
			}
			_invalidate_epoch();
			_clear_pending_jobs();
			if (!_static_body_rid.is_valid() || !_body_attached) {
				_queue(JobKind::ALLOCATE_BODY);
			}
			_queue_missing_shapes();
			_queue_reconciliation();
		} else if (missing_shapes && _jobs.empty()) {
			_queue_missing_shapes();
		}
		_run_budget();
		return;
	}
	IS_INIT(VOID);
	if (!_initialized) {
		return;
	}
	if (p_rebuild && !is_dynamic_mode()) {
		build();
		return;
	}
	int time = Time::get_singleton()->get_ticks_usec();
	real_t spacing = _terrain->get_vertex_spacing();

	if (is_dynamic_mode()) {
		// Snap descaled position to a _shape_size grid (eg. multiples of 16)
		Vector2i snapped_pos = _snap_to_grid(_terrain->get_collision_target_position() / spacing);
		LOG(EXTREME, "Updating collision at ", snapped_pos);

		// Return if target hasn't moved to next grid slot
		if (!p_rebuild && (_last_snapped_pos - snapped_pos).length_squared() == 0) {
			return;
		}

		LOG(EXTREME, "---- 1. Defining area as a radius on a grid ----");
		// Create a 0-N grid, center on snapped_pos
		PackedInt32Array grid;
		int grid_width = _radius * 2 / _shape_size; // 64*2/16 = 8
		grid_width = int_ceil_pow2(grid_width, 4);
		grid.resize(grid_width * grid_width);
		grid.fill(-1);
		Vector2i grid_offset = -V2I(grid_width / 2); // offset # cells to center of grid
		Vector2i grid_corner = snapped_pos + grid_offset * _shape_size; // Top left of grid
		LOG(EXTREME, "New snapped_pos: ", snapped_pos);
		LOG(EXTREME, "grid_corner: ", grid_corner);
		LOG(EXTREME, "radius: ", _radius, ", grid_width: ", grid_width, ", grid_offset: ", grid_offset, ", # cells: ", grid.size());
		LOG(EXTREME, "shape_size: ", _shape_size);

		LOG(EXTREME, "---- 2. Checking existing shapes ----");
		// If shape is within area, skip
		// Else, mark unused

		// Stores index into _shapes array
		TypedArray<int> inactive_shape_ids;

		real_t radius_sqr = real_t(_radius * _radius);
		Vector2i shape_offset = V2I(_shape_size / 2); // offset meters to top left corner of shape
		int shape_count = is_editor_mode() ? _shapes.size() : PS->body_get_shape_count(_static_body_rid);
		for (int i = 0; i < shape_count; i++) {
			Vector3 shape_global_pos = _shape_get_position(i);
			if (p_rebuild || shape_global_pos.x > 1e20f) {
				inactive_shape_ids.push_back(i);
				_shape_set_disabled(i, true);
				LOG(EXTREME, "Shape ", i, " marked inactive (rebuild or out of bounds)");
				continue;
			}

			// Descale global position of shape center
			Vector3 shape_center = shape_global_pos / spacing;
			// Unique key: Top left corner of shape, snapped to grid
			Vector2i shape_pos = _snap_to_grid(v3v2i(shape_center) - shape_offset);
			if (v3v2i(shape_center).distance_squared_to(snapped_pos) <= radius_sqr) {
				// Get index into shape array
				Vector2i grid_loc = (shape_pos - grid_corner) / _shape_size;
				int idx = grid_loc.y * grid_width + grid_loc.x;
				if (idx >= 0 && idx < grid.size()) {
					grid[idx] = i;
					_shape_set_disabled(i, false);
					LOG(EXTREME, "Shape ", i, ": shape_center: ", shape_center, ", shape_pos: ", shape_pos, ", grid_loc: ",
							grid_loc, ", index: ", idx, " active");
					continue;
				}
			}

			inactive_shape_ids.push_back(i);
			_shape_set_disabled(i, true);
			LOG(EXTREME, "Shape ", i, ": shape_center: ", shape_center, ", shape_pos: ", shape_pos,
					" out of bounds, marking inactive");
		}
		LOG(EXTREME, "_inactive_shapes size: ", inactive_shape_ids.size());

		LOG(EXTREME, "---- 3. Review grid cells in area ----");
		// If cell is full, skip
		// Else assign shape and form it

		for (int i = 0; i < grid.size(); i++) {
			Vector2i grid_loc(i % grid_width, i / grid_width);
			if (!p_rebuild && grid[i] >= 0) {
				LOG(EXTREME, "grid[", i, ":", grid_loc, "] already active, skipping");
				continue;
			}

			// Unique key: Top left corner of shape, snapped to grid
			Vector2i shape_pos = grid_corner + grid_loc * _shape_size;
			Vector2i shape_center = shape_pos + shape_offset;
			if (shape_center.distance_squared_to(snapped_pos) > radius_sqr) {
				LOG(EXTREME, "grid[", i, ":", grid_loc, "] shape_pos : ", shape_pos, " out of circle, skipping");
				continue;
			}
			if (inactive_shape_ids.size() == 0) {
				LOG(ERROR, "No more unused shapes! Aborting!");
				break;
			}
			Dictionary shape_data = _get_shape_data(shape_pos, _shape_size);
			if (shape_data.is_empty()) {
				LOG(EXTREME, "grid[", i, ":", grid_loc, "] shape_pos : ", shape_pos, " No region or all holes");
				continue;
			}
			int shape_id = inactive_shape_ids.pop_back();
			Transform3D xform = shape_data["xform"];
			LOG(EXTREME, "grid[", i, ":", grid_loc, "] shape_pos : ", shape_pos, " act ", v3v2i(xform.origin) - shape_offset, " placing shape id ", shape_id);
			xform.scale(Vector3(spacing, 1.f, spacing));
			_shape_set_transform(shape_id, xform);
			_shape_set_disabled(shape_id, false);
			_shape_set_data(shape_id, shape_data);
		}
		_last_snapped_pos = snapped_pos;
		LOG(EXTREME, "Setting _last_snapped_pos: ", _last_snapped_pos);
		LOG(EXTREME, "inactive_shape_ids size: ", inactive_shape_ids.size());

	} else {
		// Full collision
		int region_size = _terrain->get_region_size();
		TypedArray<Vector2i> region_locs = _terrain->get_data()->get_region_locations();
		// Shape i serves region_locs[i] only while the list is the one build() placed. A region added or
		// removed without update_maps() changes the count or shifts a placed shape, so rebuild now.
		int shape_count = is_editor_mode() ? int(_shapes.size()) : PS->body_get_shape_count(_static_body_rid);
		bool list_changed = shape_count != region_locs.size();
		for (int i = 0; !list_changed && i < shape_count; i++) {
			Vector3 placed = _shape_get_position(i);
			Vector2i region_loc = region_locs[i];
			Vector3 expected = v2iv3(region_loc * region_size + V2I(region_size / 2)) * Vector3(spacing, 1.f, spacing);
			list_changed = placed.x < 1e20f && !placed.is_equal_approx(expected);
		}
		if (list_changed) {
			LOG(DEBUG, "Region list changed since build(). Rebuilding");
			build();
			return;
		}
		for (int i = 0; i < region_locs.size(); i++) {
			Vector2i region_loc = region_locs[i];
			if (p_region_loc != V2I_MAX && region_loc != p_region_loc) {
				continue;
			}
			Vector2i shape_pos = region_loc * region_size;
			Dictionary shape_data = _get_shape_data(shape_pos, region_size);
			if (shape_data.is_empty()) {
				// Disable the shape so it keeps no stale data. Only a missing or deleted region is an error.
				_shape_set_disabled(i, true);
				const Terrain3DRegion *region = _terrain->get_data()->get_region_ptr(region_loc);
				if (!region || region->is_deleted()) {
					LOG(ERROR, "Can't get shape data for ", region_loc);
				}
				continue;
			}
			Transform3D xform = shape_data["xform"];
			xform.scale(Vector3(spacing, 1.f, spacing));
			_shape_set_transform(i, xform);
			_shape_set_disabled(i, false);
			_shape_set_data(i, shape_data);
		}
	}
	LOG(EXTREME, "Collision update time: ", Time::get_singleton()->get_ticks_usec() - time, " us");
}

void Terrain3DCollision::destroy() {
	if (_incremental_owned()) {
		begin_retirement();
		if (_terrain && _terrain->is_inside_tree() && _terrain->get_tree() && _terrain->get_tree()->get_root()) {
			_cleanup_tree = _terrain->get_tree();
			_terrain = nullptr;
			_schedule_incremental_cleanup();
			if (_incremental_cleanup) {
				return;
			}
			_terrain = nullptr;
		}
	}
	_initialized = false;
	_last_snapped_pos = V2I_MAX;

	// Physics Server
	if (_static_body_rid.is_valid()) {
		// Shape IDs change as they are freed, so it's not safe to iterate over them while freeing.
		while (PS->body_get_shape_count(_static_body_rid) > 0) {
			RID rid = PS->body_get_shape(_static_body_rid, 0);
			LOG(DEBUG, "Freeing CollisionShape RID ", rid);
			PS->free_rid(rid);
		}

		LOG(DEBUG, "Freeing StaticBody RID");
		PS->free_rid(_static_body_rid);
		_static_body_rid = RID();
	}

	// Scene Tree
	for (int i = 0; i < _shapes.size(); i++) {
		CollisionShape3D *shape = _shapes[i];
		LOG(DEBUG, "Freeing CollisionShape3D ", i, " ", shape->get_name());
		remove_from_tree(shape);
		memdelete_safely(shape);
	}
	_shapes.clear();
	if (_static_body) {
		LOG(DEBUG, "Freeing StaticBody3D");
		remove_from_tree(_static_body);
		memdelete_safely(_static_body);
	}
}

void Terrain3DCollision::set_mode(const CollisionMode p_mode) {
	bool changed = _mode != p_mode;
	_mode = p_mode;
	if (!changed && !_incremental_runtime()) {
		return;
	}
	LOG(INFO, "Setting collision mode: ", p_mode);
	if (_incremental_runtime()) {
		build();
		return;
	}
	if (is_enabled()) {
		build();
	} else {
		destroy();
	}
}

void Terrain3DCollision::set_shape_size(const uint16_t p_size) {
	uint16_t size = CLAMP(p_size, 8, 64);
	size = int_round_mult(size, uint16_t(8));
	SET_IF_DIFF(_shape_size, size);
	LOG(INFO, "Setting collision dynamic shape size: ", _shape_size);
	if (_incremental_runtime()) {
		_configure_runtime();
		_invalidate_epoch();
		_clear_pending_jobs();
		if (!_static_body_rid.is_valid() || !_body_attached) {
			_queue(JobKind::ALLOCATE_BODY);
		}
		_queue_missing_shapes();
		_queue_reconciliation();
		return;
	}
	// Ensure size:radius always results in at least one valid shape
	if (_shape_size > _radius - 8) {
		set_radius(_shape_size + 16);
	} else if (is_dynamic_mode()) {
		build();
	}
}

void Terrain3DCollision::set_radius(const uint16_t p_radius) {
	uint16_t radius = CLAMP(p_radius, 16, 256);
	radius = int_ceil_pow2(radius, uint16_t(16));
	SET_IF_DIFF(_radius, radius);
	LOG(INFO, "Setting collision dynamic radius: ", _radius);
	if (_incremental_runtime()) {
		_configure_runtime();
		_invalidate_epoch();
		_clear_pending_jobs();
		if (!_static_body_rid.is_valid() || !_body_attached) {
			_queue(JobKind::ALLOCATE_BODY);
		}
		_queue_missing_shapes();
		_queue_reconciliation();
		return;
	}
	// Ensure size:radius always results in at least one valid shape
	if (_radius < _shape_size + 8) {
		set_shape_size(_radius - 8);
	} else if (_shape_size < 16 && _radius > 128) {
		set_shape_size(16);
	} else if (is_dynamic_mode()) {
		build();
	}
}

void Terrain3DCollision::set_layer(const uint32_t p_layers) {
	SET_IF_DIFF(_layer, p_layers);
	LOG(INFO, "Setting collision layers: ", p_layers);
	if (is_editor_mode()) {
		if (_static_body) {
			_static_body->set_collision_layer(_layer);
		}
	} else {
		if (_static_body_rid.is_valid()) {
			PS->body_set_collision_layer(_static_body_rid, _layer);
		}
	}
}

void Terrain3DCollision::set_mask(const uint32_t p_mask) {
	SET_IF_DIFF(_mask, p_mask);
	LOG(INFO, "Setting collision mask: ", p_mask);
	if (is_editor_mode()) {
		if (_static_body) {
			_static_body->set_collision_mask(_mask);
		}
	} else {
		if (_static_body_rid.is_valid()) {
			PS->body_set_collision_mask(_static_body_rid, _mask);
		}
	}
}

void Terrain3DCollision::set_priority(const real_t p_priority) {
	SET_IF_DIFF(_priority, p_priority);
	LOG(INFO, "Setting collision priority: ", p_priority);
	if (is_editor_mode()) {
		if (_static_body) {
			_static_body->set_collision_priority(_priority);
		}
	} else {
		if (_static_body_rid.is_valid()) {
			PS->body_set_collision_priority(_static_body_rid, _priority);
		}
	}
}

void Terrain3DCollision::set_physics_material(const Ref<PhysicsMaterial> &p_mat) {
	if (_physics_material == p_mat) {
		return;
	}
	if (_physics_material.is_valid()) {
		if (_physics_material->is_connected("changed", callable_mp(this, &Terrain3DCollision::_reload_physics_material))) {
			LOG(DEBUG, "Disconnecting _physics_material::changed signal to _reload_physics_material()");
			_physics_material->disconnect("changed", callable_mp(this, &Terrain3DCollision::_reload_physics_material));
		}
	}
	_physics_material = p_mat;
	LOG(INFO, "Setting physics material: ", p_mat);
	if (_physics_material.is_valid()) {
		LOG(DEBUG, "Connecting _physics_material::changed signal to _reload_physics_material()");
		_physics_material->connect("changed", callable_mp(this, &Terrain3DCollision::_reload_physics_material));
	}
	_reload_physics_material();
}

void Terrain3DCollision::set_job_budget(int p_jobs) {
	int next = MAX(0, p_jobs);
	if (_job_budget == next) {
		return;
	}
	_job_budget = next;
	if (_job_budget == 0 && _incremental_owned()) {
		begin_retirement();
	}
}

Dictionary Terrain3DCollision::step_job() {
	uint64_t started = Time::get_singleton()->get_ticks_usec();
	Dictionary result;
	if (OS::get_singleton()->get_thread_caller_id() != OS::get_singleton()->get_main_thread_id()) {
		result["operation"] = "rejected";
		result["pending"] = !_jobs.empty();
		result["usec"] = 0;
		result["error"] = ERR_UNAVAILABLE;
		return result;
	}
	if (!_step_one()) {
		result = _job_result("idle", Time::get_singleton()->get_ticks_usec() - started);
		result["error"] = OK;
		return result;
	}
	result = _job_result(_last_operation, Time::get_singleton()->get_ticks_usec() - started);
	result["usec"] = Time::get_singleton()->get_ticks_usec() - started;
	result["error"] = OK;
	return result;
}

void Terrain3DCollision::begin_pause() {
	if (_runtime_state != RuntimeState::READY && _runtime_state != RuntimeState::BUILDING && _runtime_state != RuntimeState::PAUSED) {
		return;
	}
	_runtime_state = RuntimeState::PAUSED;
	_automatic_allowed = false;
	_invalidate_epoch();
	_clear_pending_jobs();
	_queue_disable_all();
}

void Terrain3DCollision::resume_jobs() {
	if (_job_budget <= 0 || _mode != DYNAMIC_GAME || IS_EDITOR) {
		return;
	}
	_runtime_state = RuntimeState::READY;
	_automatic_allowed = true;
	_initialized = true;
	_invalidate_epoch();
	_clear_pending_jobs();
	if (!_static_body_rid.is_valid() || !_body_attached) {
		_queue(JobKind::ALLOCATE_BODY);
	}
	_queue_missing_shapes();
	_queue_reconciliation();
}

void Terrain3DCollision::begin_retirement() {
	if (!_incremental_owned() || _runtime_state == RuntimeState::RETIRING) {
		return;
	}
	_runtime_state = RuntimeState::RETIRING;
	_automatic_allowed = false;
	_invalidate_epoch();
	_clear_pending_jobs();
	_queue_retirement();
}

Dictionary Terrain3DCollision::get_job_status() const {
	Dictionary status;
	status["pending"] = !_jobs.empty();
	status["shape_count"] = _shape_count();
	status["body_rid"] = _static_body_rid.is_valid() ? _static_body_rid.get_id() : 0;
	status["allocated_slots"] = _allocated_slots;
	status["completed_job_count"] = _completed_jobs;
	status["max_job_usec"] = _max_job_usec;
	status["max_page_samples"] = _max_page_samples;
	status["operations"] = _operation_stats.duplicate(true);
	status["paused"] = _runtime_state == RuntimeState::PAUSED;
	status["retiring"] = _runtime_state == RuntimeState::RETIRING;
	status["error"] = OK;
	return status;
}

RID Terrain3DCollision::get_rid() const {
	if (!is_editor_mode()) {
		return _static_body_rid;
	} else {
		if (_static_body) {
			return _static_body->get_rid();
		}
	}
	return RID();
}

///////////////////////////
// Protected Functions
///////////////////////////

void Terrain3DCollision::_bind_methods() {
	BIND_ENUM_CONSTANT(DISABLED);
	BIND_ENUM_CONSTANT(DYNAMIC_GAME);
	BIND_ENUM_CONSTANT(DYNAMIC_EDITOR);
	BIND_ENUM_CONSTANT(FULL_GAME);
	BIND_ENUM_CONSTANT(FULL_EDITOR);

	ClassDB::bind_method(D_METHOD("build"), &Terrain3DCollision::build);
	ClassDB::bind_method(D_METHOD("update", "region_location", "rebuild"), &Terrain3DCollision::update, DEFVAL(V2I_MAX), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("destroy"), &Terrain3DCollision::destroy);
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &Terrain3DCollision::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &Terrain3DCollision::get_mode);
	ClassDB::bind_method(D_METHOD("is_enabled"), &Terrain3DCollision::is_enabled);
	ClassDB::bind_method(D_METHOD("is_editor_mode"), &Terrain3DCollision::is_editor_mode);
	ClassDB::bind_method(D_METHOD("is_dynamic_mode"), &Terrain3DCollision::is_dynamic_mode);

	ClassDB::bind_method(D_METHOD("set_shape_size", "size"), &Terrain3DCollision::set_shape_size);
	ClassDB::bind_method(D_METHOD("get_shape_size"), &Terrain3DCollision::get_shape_size);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &Terrain3DCollision::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &Terrain3DCollision::get_radius);
	ClassDB::bind_method(D_METHOD("set_layer", "layers"), &Terrain3DCollision::set_layer);
	ClassDB::bind_method(D_METHOD("get_layer"), &Terrain3DCollision::get_layer);
	ClassDB::bind_method(D_METHOD("set_mask", "mask"), &Terrain3DCollision::set_mask);
	ClassDB::bind_method(D_METHOD("get_mask"), &Terrain3DCollision::get_mask);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &Terrain3DCollision::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &Terrain3DCollision::get_priority);
	ClassDB::bind_method(D_METHOD("set_physics_material", "material"), &Terrain3DCollision::set_physics_material);
	ClassDB::bind_method(D_METHOD("get_physics_material"), &Terrain3DCollision::get_physics_material);
	ClassDB::bind_method(D_METHOD("get_rid"), &Terrain3DCollision::get_rid);
	ClassDB::bind_method(D_METHOD("set_job_budget", "jobs"), &Terrain3DCollision::set_job_budget);
	ClassDB::bind_method(D_METHOD("step_job"), &Terrain3DCollision::step_job);
	ClassDB::bind_method(D_METHOD("begin_pause"), &Terrain3DCollision::begin_pause);
	ClassDB::bind_method(D_METHOD("resume_jobs"), &Terrain3DCollision::resume_jobs);
	ClassDB::bind_method(D_METHOD("begin_retirement"), &Terrain3DCollision::begin_retirement);
	ClassDB::bind_method(D_METHOD("get_job_status"), &Terrain3DCollision::get_job_status);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_size", PROPERTY_HINT_RANGE, "8,64,8"), "set_shape_size", "get_shape_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radius", PROPERTY_HINT_RANGE, "16,256,16"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_layer", "get_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_mask", "get_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "priority", PROPERTY_HINT_RANGE, "0.1,256,.1"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial"), "set_physics_material", "get_physics_material");
}

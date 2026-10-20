// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_COLLISION_CLASS_H
#define TERRAIN3D_COLLISION_CLASS_H

#include <deque>

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/physics_material.hpp>
#include <godot_cpp/classes/static_body3d.hpp>

#include "constants.h"
#include "terrain_3d_util.h"

class Terrain3D;

class Terrain3DCollision : public Object {
	GDCLASS(Terrain3DCollision, Object);
	CLASS_NAME();

public: // Constants
	enum CollisionMode {
		DISABLED,
		DYNAMIC_GAME,
		DYNAMIC_EDITOR,
		FULL_GAME,
		FULL_EDITOR,
	};

private:
	Terrain3D *_terrain = nullptr;

	// Public settings
	CollisionMode _mode = DYNAMIC_GAME;
	uint16_t _shape_size = 16;
	uint16_t _radius = 64;
	uint32_t _layer = 1;
	uint32_t _mask = 1;
	real_t _priority = 1.f;
	Ref<PhysicsMaterial> _physics_material;

	// Work data
	RID _static_body_rid; // Physics Server Static Body
	StaticBody3D *_static_body = nullptr; // Editor mode StaticBody3D
	std::vector<CollisionShape3D *> _shapes; // All CollisionShape3Ds
	struct ShapeSlot {
		RID rid;
		Vector2i position = V2I_MAX;
		uint64_t epoch = 0;
		bool installed = false;
		bool enabled = false;
	};
	enum class JobKind {
		NONE,
		ALLOCATE_BODY,
		ALLOCATE_SHAPE,
		RECONCILE,
		SAMPLE,
		UPLOAD,
		ACTIVATE,
		DISABLE,
		FREE_SHAPE,
		FREE_BODY,
	};
	enum class RuntimeState {
		LEGACY,
		BUILDING,
		READY,
		PAUSED,
		RETIRING,
	};
	struct Job {
		JobKind kind = JobKind::NONE;
		int slot = -1;
		Vector2i position = V2I_MAX;
		uint64_t epoch = 0;
	};

	RuntimeState _runtime_state = RuntimeState::LEGACY;
	int _job_budget = 0;
	uint64_t _epoch = 1;
	int _allocated_slots = 0;
	int _required_slots = 0;
	int _grid_width = 0;
	bool _body_attached = false;
	bool _automatic_allowed = true;
	bool _incremental_cleanup = false;
	uint64_t _completed_jobs = 0;
	uint64_t _max_job_usec = 0;
	int _max_page_samples = 0;
	String _last_operation = "idle";
	Dictionary _pending_page;
	int _pending_page_slot = -1;
	uint64_t _pending_page_epoch = 0;
	Transform3D _pending_page_transform;
	Dictionary _operation_stats;
	std::vector<ShapeSlot> _slots;
	std::deque<Job> _jobs;
	SceneTree *_cleanup_tree = nullptr;

	bool _incremental_runtime() const;
	bool _incremental_owned() const;
	int _shape_count() const;
	void _invalidate_epoch();
	void _record_job(const String &p_operation, uint64_t p_usec, int p_page_samples = 0);
	Dictionary _job_result(const String &p_operation, uint64_t p_usec) const;
	void _queue(JobKind p_kind, int p_slot = -1, const Vector2i &p_position = V2I_MAX);
	void _push_front(const Job &p_job);
	void _clear_pending_jobs();
	void _queue_missing_shapes();
	void _queue_reconciliation();
	void _queue_disable_all();
	void _queue_retirement();
	void _configure_runtime();
	bool _step_one();
	void _run_budget();
	void _detach_body();
	void _schedule_incremental_cleanup();
	void _cleanup_step();
	void _mark_slot_unused(int p_slot);
	void _set_slot_disabled(int p_slot, bool p_disabled);
	void _set_slot_transform(int p_slot, const Transform3D &p_transform);
	void _set_slot_data(int p_slot, const Dictionary &p_data);
	Dictionary _sample_shape_page(const Vector2i &p_position, int p_size, int &r_samples);
	bool _initialized = false;
	Vector2i _last_snapped_pos = V2I_MAX;

	Vector2i _snap_to_grid(const Vector2i &p_pos) const;
	Vector2i _snap_to_grid(const Vector3 &p_pos) const;
	Dictionary _get_shape_data(const Vector2i &p_position, const int p_size);

	void _shape_set_disabled(const int p_shape_id, const bool p_disabled);
	void _shape_set_transform(const int p_shape_id, const Transform3D &p_xform);
	Vector3 _shape_get_position(const int p_shape_id) const;
	void _shape_set_data(const int p_shape_id, const Dictionary &p_dict);

	void _reload_physics_material();

public:
	Terrain3DCollision() {}
	~Terrain3DCollision() { destroy(); }
	void initialize(Terrain3D *p_terrain);
	void build();
	void reset_target_position();
	void update(const Vector2i &p_region_loc = V2I_MAX, const bool p_rebuild = false);
	void destroy();
	bool is_cleanup_deferred() const { return _incremental_cleanup; }
	void set_job_budget(int p_jobs);
	Dictionary step_job();
	void begin_pause();
	void resume_jobs();
	void begin_retirement();
	Dictionary get_job_status() const;

	void set_mode(const CollisionMode p_mode);
	CollisionMode get_mode() const { return _mode; }
	bool is_enabled() const { return _mode > DISABLED; }
	bool is_editor_mode() const { return _mode == DYNAMIC_EDITOR || _mode == FULL_EDITOR; }
	bool is_dynamic_mode() const { return _mode == DYNAMIC_GAME || _mode == DYNAMIC_EDITOR; }

	void set_shape_size(const uint16_t p_size);
	uint16_t get_shape_size() const { return _shape_size; }
	void set_radius(const uint16_t p_radius);
	uint16_t get_radius() const { return _radius; }
	void set_layer(const uint32_t p_layers);
	uint32_t get_layer() const { return _layer; }
	void set_mask(const uint32_t p_mask);
	uint32_t get_mask() const { return _mask; }
	void set_priority(const real_t p_priority);
	real_t get_priority() const { return _priority; }
	void set_physics_material(const Ref<PhysicsMaterial> &p_mat);
	Ref<PhysicsMaterial> get_physics_material() { return _physics_material; }
	RID get_rid() const;

protected:
	static void _bind_methods();
};

using CollisionMode = Terrain3DCollision::CollisionMode;
VARIANT_ENUM_CAST(Terrain3DCollision::CollisionMode);

inline Vector2i Terrain3DCollision::_snap_to_grid(const Vector2i &p_pos) const {
	return Vector2i(int_round_mult(p_pos.x, int32_t(_shape_size)),
			int_round_mult(p_pos.y, int32_t(_shape_size)));
}

inline Vector2i Terrain3DCollision::_snap_to_grid(const Vector3 &p_pos) const {
	return Vector2i(Math::floor(p_pos.x / real_t(_shape_size) + 0.5f),
				   Math::floor(p_pos.z / real_t(_shape_size) + 0.5f)) *
			_shape_size;
}

#endif // TERRAIN3D_COLLISION_CLASS_H
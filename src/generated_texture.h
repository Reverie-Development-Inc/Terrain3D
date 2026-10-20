// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef GENERATEDTEXTURE_CLASS_H
#define GENERATEDTEXTURE_CLASS_H

#include <godot_cpp/classes/image.hpp>

#include "constants.h"

class GeneratedTexture {
	CLASS_NAME_STATIC("Terrain3DGenTex");

private:
	RID _rid = RID();
	Ref<Image> _image;
	bool _dirty = false;
	int _size = 0;
	Vector2i _layer_size;
	Image::Format _layer_format = Image::FORMAT_MAX;
	bool _layer_mipmaps = false;

public:
	void clear();
	bool is_dirty() const { return _dirty; }
	RID create(const TypedArray<Image> &p_layers);
	Error allocate_layers(const int p_capacity, const Ref<Image> &p_blank);
	Error update_layer(const Ref<Image> &p_image, const int p_layer);
	void update(const Ref<Image> &p_image, const int p_layer);
	RID create(const Ref<Image> &p_image);
	Ref<Image> get_image() const { return _image; }
	RID get_rid() const { return _rid; }
	int size() const { return _size; }
};

#endif // GENERATEDTEXTURE_CLASS_H
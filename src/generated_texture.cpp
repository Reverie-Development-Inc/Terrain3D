// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/rendering_server.hpp>

#include "generated_texture.h"
#include "logger.h"
#include "terrain_3d.h"

///////////////////////////
// Public Functions
///////////////////////////

void GeneratedTexture::clear() {
	if (_rid.is_valid()) {
		LOG(EXTREME, "GeneratedTexture freeing ", _rid);
		RS->free_rid(_rid);
	}
	if (_image.is_valid()) {
		LOG(EXTREME, "GeneratedTexture unref image", _image);
		_image.unref();
	}
	_rid = RID();
	_dirty = true;
	_size = 0;
	_layer_size = Vector2i();
	_layer_format = Image::FORMAT_MAX;
	_layer_mipmaps = false;
}

RID GeneratedTexture::create(const TypedArray<Image> &p_layers) {
	if (!p_layers.is_empty()) {
		if (Terrain3D::debug_level >= DEBUG) {
			LOG(EXTREME, "RenderingServer creating Texture2DArray, layers size: ", p_layers.size());
			for (int i = 0; i < p_layers.size(); i++) {
				Ref<Image> img = p_layers[i];
				LOG(EXTREME, i, ": ", img, ", empty: ", img->is_empty(), ", size: ", img->get_size(), ", format: ", img->get_format());
			}
		}
		_rid = RS->texture_2d_layered_create(p_layers, RenderingServer::TEXTURE_LAYERED_2D_ARRAY);
		_dirty = false;
		_size = p_layers.size();
	} else {
		clear();
	}
	return _rid;
}

// The temporary layer list repeats one blank image; empty slots retain no CPU image.
Error GeneratedTexture::allocate_layers(const int p_capacity, const Ref<Image> &p_blank) {
	if (_rid.is_valid()) {
		return ERR_ALREADY_IN_USE;
	}
	if (p_capacity <= 0 || p_blank.is_null() || p_blank->is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	TypedArray<Image> layers;
	layers.resize(p_capacity);
	for (int i = 0; i < p_capacity; i++) {
		layers[i] = p_blank;
	}
	if (!create(layers).is_valid()) {
		_dirty = true;
		return ERR_CANT_CREATE;
	}
	_layer_size = p_blank->get_size();
	_layer_format = p_blank->get_format();
	_layer_mipmaps = p_blank->has_mipmaps();
	return OK;
}

Error GeneratedTexture::update_layer(const Ref<Image> &p_image, const int p_layer) {
	if (!_rid.is_valid()) {
		return ERR_UNCONFIGURED;
	}
	if (p_layer < 0 || p_layer >= _size || p_image.is_null() || p_image->is_empty() ||
			p_image->get_size() != _layer_size || p_image->get_format() != _layer_format ||
			p_image->has_mipmaps() != _layer_mipmaps) {
		return ERR_INVALID_PARAMETER;
	}
	update(p_image, p_layer);
	return OK;
}

void GeneratedTexture::update(const Ref<Image> &p_image, const int p_layer) {
	LOG(EXTREME, "RenderingServer updating Texture2DArray at index: ", p_layer);
	RS->texture_2d_update(_rid, p_image, p_layer);
}

RID GeneratedTexture::create(const Ref<Image> &p_image) {
	LOG(EXTREME, "RenderingServer creating Texture2D");
	_image = p_image;
	_rid = RS->texture_2d_create(_image);
	_dirty = false;
	return _rid;
}

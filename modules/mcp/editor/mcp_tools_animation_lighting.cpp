/**************************************************************************/
/*  mcp_tools_animation_lighting.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_builtin_tools.h"

#include "../mcp_server.h"
#include "../mcp_variant_codec.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/2d/light_2d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "scene/resources/3d/sky_material.h"

namespace {

Node *_find_scene_node(const String &p_path, String &r_err) {
	if (!EditorInterface::get_singleton()) {
		r_err = "EditorInterface is not available.";
		return nullptr;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		r_err = "No scene is currently open.";
		return nullptr;
	}
	String p = p_path.strip_edges();
	if (p.is_empty() || p == "." || p == "/") {
		return root;
	}
	Node *n = root->get_node_or_null(NodePath(p));
	if (!n) {
		r_err = vformat("Node not found at '%s'.", p_path);
	}
	return n;
}

AnimationPlayer *_resolve_anim_player(const String &p_path, String &r_err) {
	Node *n = _find_scene_node(p_path, r_err);
	if (!n) {
		return nullptr;
	}
	AnimationPlayer *ap = Object::cast_to<AnimationPlayer>(n);
	if (!ap) {
		r_err = vformat("Node '%s' is a %s, not an AnimationPlayer.", p_path, n->get_class());
		return nullptr;
	}
	return ap;
}

Ref<AnimationLibrary> _get_or_create_library(AnimationPlayer *p_ap, const String &p_library_name) {
	StringName lib_sn(p_library_name);
	if (p_ap->has_animation_library(lib_sn)) {
		return p_ap->get_animation_library(lib_sn);
	}
	Ref<AnimationLibrary> lib;
	lib.instantiate();
	p_ap->add_animation_library(lib_sn, lib);
	return lib;
}

Animation::TrackType _parse_track_type(const String &p_str, bool &r_ok) {
	r_ok = true;
	String s = p_str.strip_edges().to_lower();
	if (s == "value") {
		return Animation::TYPE_VALUE;
	}
	if (s == "position_3d") {
		return Animation::TYPE_POSITION_3D;
	}
	if (s == "rotation_3d") {
		return Animation::TYPE_ROTATION_3D;
	}
	if (s == "scale_3d") {
		return Animation::TYPE_SCALE_3D;
	}
	if (s == "blend_shape") {
		return Animation::TYPE_BLEND_SHAPE;
	}
	if (s == "method") {
		return Animation::TYPE_METHOD;
	}
	if (s == "bezier") {
		return Animation::TYPE_BEZIER;
	}
	if (s == "audio") {
		return Animation::TYPE_AUDIO;
	}
	if (s == "animation") {
		return Animation::TYPE_ANIMATION;
	}
	r_ok = false;
	return Animation::TYPE_VALUE;
}

String _track_type_name(Animation::TrackType p_type) {
	switch (p_type) {
		case Animation::TYPE_VALUE:
			return "value";
		case Animation::TYPE_POSITION_3D:
			return "position_3d";
		case Animation::TYPE_ROTATION_3D:
			return "rotation_3d";
		case Animation::TYPE_SCALE_3D:
			return "scale_3d";
		case Animation::TYPE_BLEND_SHAPE:
			return "blend_shape";
		case Animation::TYPE_METHOD:
			return "method";
		case Animation::TYPE_BEZIER:
			return "bezier";
		case Animation::TYPE_AUDIO:
			return "audio";
		case Animation::TYPE_ANIMATION:
			return "animation";
	}
	return "unknown";
}

Dictionary _tool_animation_list(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}

	Array libs;
	LocalVector<StringName> lib_names;
	ap->get_animation_library_list(&lib_names);
	for (uint32_t li = 0; li < lib_names.size(); li++) {
		const StringName &lib_name = lib_names[li];
		Ref<AnimationLibrary> lib = ap->get_animation_library(lib_name);
		if (lib.is_null()) {
			continue;
		}
		Dictionary ld;
		ld["library"] = String(lib_name);
		Array anims;
		LocalVector<StringName> anim_names;
		lib->get_animation_list(&anim_names);
		for (uint32_t i = 0; i < anim_names.size(); i++) {
			Ref<Animation> anim = lib->get_animation(anim_names[i]);
			Dictionary ad;
			ad["name"] = String(anim_names[i]);
			if (anim.is_valid()) {
				ad["length"] = anim->get_length();
				ad["step"] = anim->get_step();
				ad["loop_mode"] = (int)anim->get_loop_mode();
				ad["track_count"] = anim->get_track_count();
			}
			anims.push_back(ad);
		}
		ld["animations"] = anims;
		libs.push_back(ld);
	}

	Dictionary out;
	out["autoplay"] = String(ap->get_autoplay());
	out["libraries"] = libs;
	return mcp::make_ok(out);
}

Dictionary _tool_animation_create(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	if (anim_name.is_empty()) {
		return mcp::make_error("Parameter 'animation' is required.", "invalid_argument");
	}

	Ref<AnimationLibrary> lib = _get_or_create_library(ap, lib_name);
	Ref<Animation> anim;
	anim.instantiate();
	anim->set_length(MAX(0.01, double(p_args.get("length", 1.0))));
	anim->set_step(float(p_args.get("step", 0.05)));

	String loop_str = String(p_args.get("loop_mode", "none")).strip_edges().to_lower();
	if (loop_str == "linear" || loop_str == "loop") {
		anim->set_loop_mode(Animation::LOOP_LINEAR);
	} else if (loop_str == "pingpong") {
		anim->set_loop_mode(Animation::LOOP_PINGPONG);
	} else {
		anim->set_loop_mode(Animation::LOOP_NONE);
	}

	if (lib->has_animation(anim_name)) {
		lib->remove_animation(anim_name);
	}
	Error aerr = lib->add_animation(anim_name, anim);
	if (aerr != OK) {
		return mcp::make_error(vformat("Failed to add animation '%s' (error %d).", anim_name, (int)aerr), "animation_error");
	}
	if (bool(p_args.get("autoplay", false))) {
		String full_name = lib_name.is_empty() ? anim_name : (lib_name + "/" + anim_name);
		ap->set_autoplay(full_name);
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["library"] = lib_name;
	out["animation"] = anim_name;
	out["length"] = anim->get_length();
	return mcp::make_ok(out);
}

Dictionary _tool_animation_delete(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	if (!ap->has_animation_library(lib_name)) {
		return mcp::make_error("Animation library not found.", "not_found");
	}
	Ref<AnimationLibrary> lib = ap->get_animation_library(lib_name);
	if (!lib->has_animation(anim_name)) {
		return mcp::make_error(vformat("Animation '%s' not found.", anim_name), "not_found");
	}
	lib->remove_animation(anim_name);
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	return mcp::make_ok();
}

Dictionary _tool_animation_add_track(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	String track_path = String(p_args.get("track_path", "")).strip_edges();
	String track_type_str = String(p_args.get("track_type", "value")).strip_edges();

	if (anim_name.is_empty() || track_path.is_empty()) {
		return mcp::make_error("Parameters 'animation' and 'track_path' are required.", "invalid_argument");
	}
	if (!ap->has_animation_library(lib_name)) {
		return mcp::make_error("Animation library not found.", "not_found");
	}
	Ref<Animation> anim = ap->get_animation_library(lib_name)->get_animation(anim_name);
	if (anim.is_null()) {
		return mcp::make_error(vformat("Animation '%s' not found.", anim_name), "not_found");
	}

	bool ok = false;
	Animation::TrackType tt = _parse_track_type(track_type_str, ok);
	if (!ok) {
		return mcp::make_error(vformat("Unknown track_type '%s'.", track_type_str), "invalid_argument");
	}

	int idx = anim->add_track(tt);
	anim->track_set_path(idx, NodePath(track_path));

	String interp = String(p_args.get("interpolation", "linear")).strip_edges().to_lower();
	if (interp == "nearest") {
		anim->track_set_interpolation_type(idx, Animation::INTERPOLATION_NEAREST);
	} else if (interp == "cubic") {
		anim->track_set_interpolation_type(idx, Animation::INTERPOLATION_CUBIC);
	} else {
		anim->track_set_interpolation_type(idx, Animation::INTERPOLATION_LINEAR);
	}

	if (tt == Animation::TYPE_VALUE && p_args.has("update_mode")) {
		String um = String(p_args["update_mode"]).strip_edges().to_lower();
		if (um == "discrete") {
			anim->value_track_set_update_mode(idx, Animation::UPDATE_DISCRETE);
		} else if (um == "capture") {
			anim->value_track_set_update_mode(idx, Animation::UPDATE_CAPTURE);
		} else {
			anim->value_track_set_update_mode(idx, Animation::UPDATE_CONTINUOUS);
		}
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["track_index"] = idx;
	out["track_type"] = _track_type_name(tt);
	out["track_path"] = track_path;
	return mcp::make_ok(out);
}

Dictionary _tool_animation_insert_key(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	int track_index = int(p_args.get("track_index", -1));

	if (!ap->has_animation_library(lib_name)) {
		return mcp::make_error("Animation library not found.", "not_found");
	}
	Ref<Animation> anim = ap->get_animation_library(lib_name)->get_animation(anim_name);
	if (anim.is_null()) {
		return mcp::make_error(vformat("Animation '%s' not found.", anim_name), "not_found");
	}
	if (track_index < 0 || track_index >= anim->get_track_count()) {
		return mcp::make_error(vformat("Invalid track_index %d (track_count is %d).", track_index, anim->get_track_count()), "invalid_track");
	}

	Array keys_to_insert;
	if (p_args.has("keys") && p_args["keys"].get_type() == Variant::ARRAY) {
		keys_to_insert = p_args["keys"];
	} else {
		Dictionary single;
		single["time"] = double(p_args.get("time", 0.0));
		single["value"] = p_args.get("value", Variant());
		single["transition"] = float(p_args.get("transition", 1.0));
		keys_to_insert.push_back(single);
	}

	Animation::TrackType tt = anim->track_get_type(track_index);
	int inserted = 0;

	for (int i = 0; i < keys_to_insert.size(); i++) {
		Dictionary k = keys_to_insert[i];
		double time = double(k.get("time", 0.0));
		float transition = float(k.get("transition", 1.0));
		Variant raw_val = k.get("value", Variant());

		if (tt == Animation::TYPE_POSITION_3D) {
			Vector3 v;
			if (!mcp::parse_vector3(raw_val, v)) {
				return mcp::make_error("position_3d key value must be a 3D vector [x,y,z].", "invalid_value");
			}
			anim->position_track_insert_key(track_index, time, v);
		} else if (tt == Animation::TYPE_SCALE_3D) {
			Vector3 v;
			if (!mcp::parse_vector3(raw_val, v)) {
				return mcp::make_error("scale_3d key value must be a 3D vector [x,y,z].", "invalid_value");
			}
			anim->scale_track_insert_key(track_index, time, v);
		} else if (tt == Animation::TYPE_ROTATION_3D) {
			Quaternion q;
			Vector3 euler_deg;
			if (raw_val.get_type() == Variant::ARRAY && Array(raw_val).size() == 4) {
				Array a = raw_val;
				q = Quaternion(float(a[0]), float(a[1]), float(a[2]), float(a[3]));
			} else if (mcp::parse_vector3(raw_val, euler_deg)) {
				q = Quaternion::from_euler(Vector3(Math::deg_to_rad(euler_deg.x), Math::deg_to_rad(euler_deg.y), Math::deg_to_rad(euler_deg.z)));
			} else {
				return mcp::make_error("rotation_3d key value must be quaternion [x,y,z,w] or euler degrees [x,y,z].", "invalid_value");
			}
			anim->rotation_track_insert_key(track_index, time, q);
		} else if (tt == Animation::TYPE_AUDIO) {
			String stream_path = mcp::normalize_res_path(String(raw_val));
			Ref<Resource> stream = ResourceLoader::load(stream_path);
			if (stream.is_null()) {
				return mcp::make_error(vformat("Could not load audio stream '%s'.", stream_path), "load_failed");
			}
			anim->audio_track_insert_key(track_index, time, stream, float(k.get("start_offset", 0.0)), float(k.get("end_offset", 0.0)));
		} else if (tt == Animation::TYPE_ANIMATION) {
			anim->animation_track_insert_key(track_index, time, StringName(String(raw_val)));
		} else {
			Variant converted = mcp::json_to_variant(raw_val);
			anim->track_insert_key(track_index, time, converted, transition);
		}
		inserted++;
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["track_index"] = track_index;
	out["inserted_keys"] = inserted;
	out["total_keys"] = anim->track_get_key_count(track_index);
	return mcp::make_ok(out);
}

Dictionary _tool_animation_remove_key(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	int track_index = int(p_args.get("track_index", -1));
	int key_index = int(p_args.get("key_index", -1));

	if (!ap->has_animation_library(lib_name)) {
		return mcp::make_error("Animation library not found.", "not_found");
	}
	Ref<Animation> anim = ap->get_animation_library(lib_name)->get_animation(anim_name);
	if (anim.is_null() || track_index < 0 || track_index >= anim->get_track_count()) {
		return mcp::make_error("Invalid animation or track_index.", "not_found");
	}
	if (key_index >= 0 && key_index < anim->track_get_key_count(track_index)) {
		anim->track_remove_key(track_index, key_index);
	} else if (p_args.has("time")) {
		anim->track_remove_key_at_time(track_index, double(p_args["time"]));
	} else {
		return mcp::make_error("Provide a valid 'key_index' or 'time'.", "invalid_argument");
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["remaining_keys"] = anim->track_get_key_count(track_index);
	return mcp::make_ok(out);
}

Dictionary _tool_animation_get_details(const Dictionary &p_args) {
	String err;
	AnimationPlayer *ap = _resolve_anim_player(String(p_args.get("player_path", "")), err);
	if (!ap) {
		return mcp::make_error(err, "node_not_found");
	}
	String anim_name = String(p_args.get("animation", "")).strip_edges();
	String lib_name = String(p_args.get("library", "")).strip_edges();
	if (!ap->has_animation_library(lib_name)) {
		return mcp::make_error("Animation library not found.", "not_found");
	}
	Ref<Animation> anim = ap->get_animation_library(lib_name)->get_animation(anim_name);
	if (anim.is_null()) {
		return mcp::make_error(vformat("Animation '%s' not found.", anim_name), "not_found");
	}

	Array tracks;
	for (int t = 0; t < anim->get_track_count(); t++) {
		Dictionary td;
		td["index"] = t;
		td["type"] = _track_type_name(anim->track_get_type(t));
		td["path"] = String(anim->track_get_path(t));
		td["enabled"] = anim->track_is_enabled(t);
		Array keys;
		for (int k = 0; k < anim->track_get_key_count(t); k++) {
			Dictionary kd;
			kd["index"] = k;
			kd["time"] = anim->track_get_key_time(t, k);
			kd["value"] = mcp::variant_to_json(anim->track_get_key_value(t, k));
			kd["transition"] = anim->track_get_key_transition(t, k);
			keys.push_back(kd);
		}
		td["keys"] = keys;
		tracks.push_back(td);
	}

	Dictionary out;
	out["animation"] = anim_name;
	out["library"] = lib_name;
	out["length"] = anim->get_length();
	out["step"] = anim->get_step();
	out["loop_mode"] = (int)anim->get_loop_mode();
	out["tracks"] = tracks;
	return mcp::make_ok(out);
}

Dictionary _tool_lighting_add_light(const Dictionary &p_args) {
	String err;
	Node *parent = _find_scene_node(String(p_args.get("parent_path", ".")), err);
	if (!parent) {
		return mcp::make_error(err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();

	String light_type = String(p_args.get("light_type", "OmniLight3D")).strip_edges();
	String name = String(p_args.get("name", "")).strip_edges();

	if (light_type != "DirectionalLight3D" && light_type != "OmniLight3D" && light_type != "SpotLight3D" &&
			light_type != "PointLight2D" && light_type != "DirectionalLight2D") {
		return mcp::make_error("light_type must be DirectionalLight3D, OmniLight3D, SpotLight3D, PointLight2D, or DirectionalLight2D.", "invalid_argument");
	}

	Node *node = Object::cast_to<Node>(ClassDB::instantiate(light_type));
	if (!node) {
		return mcp::make_error(vformat("Failed to instantiate '%s'.", light_type), "instantiate_failed");
	}
	node->set_name(name.is_empty() ? light_type : name);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Add %s", String(node->get_name())));
		ur->add_do_method(parent, "add_child", node, true);
		ur->add_do_method(node, "set_owner", root);
		ur->add_do_reference(node);
		ur->add_undo_method(parent, "remove_child", node);
		ur->commit_action();
	} else {
		parent->add_child(node, true);
		node->set_owner(root);
	}

	if (Light3D *l3 = Object::cast_to<Light3D>(node)) {
		Vector3 v;
		if (p_args.has("position") && mcp::parse_vector3(p_args["position"], v)) {
			l3->set_position(v);
		}
		if (p_args.has("rotation_degrees") && mcp::parse_vector3(p_args["rotation_degrees"], v)) {
			l3->set_rotation_degrees(v);
		}
		Color c;
		if (p_args.has("color") && mcp::parse_color(p_args["color"], c)) {
			l3->set_color(c);
		}
		if (p_args.has("energy")) {
			l3->set_param(Light3D::PARAM_ENERGY, float(p_args["energy"]));
		}
		if (p_args.has("volumetric_fog_energy")) {
			l3->set_param(Light3D::PARAM_VOLUMETRIC_FOG_ENERGY, float(p_args["volumetric_fog_energy"]));
		}
		if (p_args.has("range")) {
			l3->set_param(Light3D::PARAM_RANGE, float(p_args["range"]));
		}
		if (p_args.has("attenuation")) {
			l3->set_param(Light3D::PARAM_ATTENUATION, float(p_args["attenuation"]));
		}
		if (p_args.has("spot_angle")) {
			l3->set_param(Light3D::PARAM_SPOT_ANGLE, float(p_args["spot_angle"]));
		}
		if (p_args.has("shadow_enabled")) {
			l3->set_shadow(bool(p_args["shadow_enabled"]));
		}
	} else if (Light2D *l2 = Object::cast_to<Light2D>(node)) {
		Vector2 v;
		if (p_args.has("position") && mcp::parse_vector2(p_args["position"], v)) {
			l2->set_position(v);
		}
		Color c;
		if (p_args.has("color") && mcp::parse_color(p_args["color"], c)) {
			l2->set_color(c);
		}
		if (p_args.has("energy")) {
			l2->set_energy(float(p_args["energy"]));
		}
		if (p_args.has("shadow_enabled")) {
			l2->set_shadow_enabled(bool(p_args["shadow_enabled"]));
		}
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["name"] = String(node->get_name());
	out["path"] = String(root->get_path_to(node));
	out["type"] = light_type;
	return mcp::make_ok(out);
}

WorldEnvironment *_find_world_environment(Node *p_node) {
	if (!p_node) {
		return nullptr;
	}
	if (WorldEnvironment *we = Object::cast_to<WorldEnvironment>(p_node)) {
		return we;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (WorldEnvironment *found = _find_world_environment(p_node->get_child(i))) {
			return found;
		}
	}
	return nullptr;
}

Dictionary _tool_lighting_configure_environment(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) {
		return mcp::make_error("EditorInterface is not available.", "editor_required");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return mcp::make_error("No scene is currently open.", "no_scene");
	}

	WorldEnvironment *we = nullptr;
	if (p_args.has("node_path")) {
		String rerr;
		we = Object::cast_to<WorldEnvironment>(_find_scene_node(String(p_args["node_path"]), rerr));
	}
	if (!we) {
		we = _find_world_environment(root);
	}
	bool created = false;
	if (!we) {
		we = memnew(WorldEnvironment);
		we->set_name("WorldEnvironment");
		root->add_child(we, true);
		we->set_owner(root);
		created = true;
	}

	Ref<Environment> env = we->get_environment();
	if (env.is_null()) {
		env.instantiate();
		we->set_environment(env);
	}

	if (p_args.has("background_mode")) {
		String bm = String(p_args["background_mode"]).strip_edges().to_lower();
		if (bm == "color") {
			env->set_background(Environment::BG_COLOR);
		} else if (bm == "sky") {
			env->set_background(Environment::BG_SKY);
			if (env->get_sky().is_null()) {
				Ref<Sky> sky;
				sky.instantiate();
				Ref<ProceduralSkyMaterial> sm;
				sm.instantiate();
				sky->set_material(sm);
				env->set_sky(sky);
			}
		} else if (bm == "clear_color") {
			env->set_background(Environment::BG_CLEAR_COLOR);
		}
	}

	Color c;
	if (p_args.has("background_color") && mcp::parse_color(p_args["background_color"], c)) {
		env->set_bg_color(c);
	}
	if (p_args.has("ambient_light_color") && mcp::parse_color(p_args["ambient_light_color"], c)) {
		env->set_ambient_light_color(c);
		env->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	}
	if (p_args.has("ambient_light_energy")) {
		env->set_ambient_light_energy(float(p_args["ambient_light_energy"]));
	}
	if (p_args.has("tonemap_mode")) {
		String tm = String(p_args["tonemap_mode"]).strip_edges().to_lower();
		if (tm == "aces") {
			env->set_tonemapper(Environment::TONE_MAPPER_ACES);
		} else if (tm == "agx") {
			env->set_tonemapper(Environment::TONE_MAPPER_AGX);
		} else if (tm == "filmic") {
			env->set_tonemapper(Environment::TONE_MAPPER_FILMIC);
		} else if (tm == "reinhard") {
			env->set_tonemapper(Environment::TONE_MAPPER_REINHARDT);
		} else {
			env->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
		}
	}
	if (p_args.has("glow_enabled")) {
		env->set_glow_enabled(bool(p_args["glow_enabled"]));
	}
	if (p_args.has("glow_intensity")) {
		env->set_glow_intensity(float(p_args["glow_intensity"]));
	}
	if (p_args.has("fog_enabled")) {
		env->set_fog_enabled(bool(p_args["fog_enabled"]));
	}
	if (p_args.has("fog_color") && mcp::parse_color(p_args["fog_color"], c)) {
		env->set_fog_light_color(c);
	}
	if (p_args.has("fog_density")) {
		env->set_fog_density(float(p_args["fog_density"]));
	}
	if (p_args.has("volumetric_fog_enabled")) {
		env->set_volumetric_fog_enabled(bool(p_args["volumetric_fog_enabled"]));
	}
	if (p_args.has("volumetric_fog_density")) {
		env->set_volumetric_fog_density(float(p_args["volumetric_fog_density"]));
	}
	if (p_args.has("volumetric_fog_albedo") && mcp::parse_color(p_args["volumetric_fog_albedo"], c)) {
		env->set_volumetric_fog_albedo(c);
	}
	if (p_args.has("ssao_enabled")) {
		env->set_ssao_enabled(bool(p_args["ssao_enabled"]));
	}
	if (p_args.has("sdfgi_enabled")) {
		env->set_sdfgi_enabled(bool(p_args["sdfgi_enabled"]));
	}
	if (p_args.has("adjustment_enabled")) {
		env->set_adjustment_enabled(bool(p_args["adjustment_enabled"]));
	}
	if (p_args.has("adjustment_brightness")) {
		env->set_adjustment_brightness(float(p_args["adjustment_brightness"]));
	}
	if (p_args.has("adjustment_contrast")) {
		env->set_adjustment_contrast(float(p_args["adjustment_contrast"]));
	}
	if (p_args.has("adjustment_saturation")) {
		env->set_adjustment_saturation(float(p_args["adjustment_saturation"]));
	}

	// Extra arbitrary Environment properties.
	if (p_args.has("properties") && p_args["properties"].get_type() == Variant::DICTIONARY) {
		Dictionary extra = p_args["properties"];
		Array keys = extra.keys();
		for (int i = 0; i < keys.size(); i++) {
			String k = String(keys[i]);
			PropertyInfo pi;
			Variant::Type hint = Variant::NIL;
			if (ClassDB::get_property_info("Environment", k, &pi)) {
				hint = pi.type;
			}
			env->set(k, mcp::json_to_variant(extra[k], hint));
		}
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["node_path"] = String(root->get_path_to(we));
	out["created"] = created;
	out["fog_enabled"] = env->is_fog_enabled();
	out["volumetric_fog_enabled"] = env->is_volumetric_fog_enabled();
	out["glow_enabled"] = env->is_glow_enabled();
	return mcp::make_ok(out);
}

Dictionary _tool_class_list(const Dictionary &p_args) {
	String inherits = String(p_args.get("inherits", "")).strip_edges();
	String search = String(p_args.get("search", "")).strip_edges().to_lower();
	bool instantiable_only = bool(p_args.get("instantiable_only", true));
	int limit = CLAMP(int(p_args.get("limit", 200)), 1, 2000);

	LocalVector<StringName> all_classes;
	if (!inherits.is_empty()) {
		if (!ClassDB::class_exists(inherits)) {
			return mcp::make_error(vformat("Base class '%s' does not exist.", inherits), "not_found");
		}
		ClassDB::get_inheriters_from_class(inherits, all_classes);
	} else {
		ClassDB::get_class_list(all_classes);
	}

	Array result;
	for (uint32_t i = 0; i < all_classes.size() && result.size() < limit; i++) {
		String cname = String(all_classes[i]);
		if (instantiable_only && !ClassDB::can_instantiate(all_classes[i])) {
			continue;
		}
		if (!search.is_empty() && !cname.to_lower().contains(search)) {
			continue;
		}
		result.push_back(cname);
	}

	Dictionary out;
	out["classes"] = result;
	out["count"] = result.size();
	return mcp::make_ok(out);
}

Dictionary _tool_class_get_info(const Dictionary &p_args) {
	String cls = String(p_args.get("class_name", "")).strip_edges();
	if (cls.is_empty() || !ClassDB::class_exists(cls)) {
		return mcp::make_error(vformat("Class '%s' does not exist in ClassDB.", cls), "not_found");
	}
	bool no_inheritance = bool(p_args.get("no_inheritance", true));

	Dictionary out;
	out["class_name"] = cls;
	out["parent_class"] = String(ClassDB::get_parent_class(cls));
	out["can_instantiate"] = ClassDB::can_instantiate(cls);

	Array props;
	List<PropertyInfo> plist;
	ClassDB::get_property_list(cls, &plist, no_inheritance);
	for (const PropertyInfo &pi : plist) {
		if (pi.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_SUBGROUP)) {
			continue;
		}
		Dictionary pd;
		pd["name"] = pi.name;
		pd["type"] = Variant::get_type_name(pi.type);
		if (!pi.hint_string.is_empty()) {
			pd["hint"] = pi.hint_string;
		}
		props.push_back(pd);
	}
	out["properties"] = props;

	Array methods;
	List<MethodInfo> mlist;
	ClassDB::get_method_list(cls, &mlist, no_inheritance);
	for (const MethodInfo &mi : mlist) {
		if (mi.name.begins_with("_")) {
			continue;
		}
		Dictionary md;
		md["name"] = mi.name;
		Array args;
		for (const PropertyInfo &ai : mi.arguments) {
			args.push_back(vformat("%s: %s", ai.name, Variant::get_type_name(ai.type)));
		}
		md["args"] = args;
		md["return_type"] = Variant::get_type_name(mi.return_val.type);
		methods.push_back(md);
	}
	out["methods"] = methods;

	Array signals;
	List<MethodInfo> slist;
	ClassDB::get_signal_list(cls, &slist, no_inheritance);
	for (const MethodInfo &si : slist) {
		signals.push_back(si.name);
	}
	out["signals"] = signals;
	return mcp::make_ok(out);
}

} // namespace

void mcp_register_animation_lighting_tools(MinaMCPServer *p_server) {
	using namespace mcp;

	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer in the edited scene.");
		Array req;
		req.push_back("player_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_list";
		t.title = "List Animations";
		t.description = "List all animation libraries and animations on an AnimationPlayer node.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_list;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name (e.g. 'open_door', 'flicker').");
		props["library"] = schema_string("AnimationLibrary name (empty string '' for default library).");
		props["length"] = schema_number("Animation duration in seconds (default: 1.0).");
		props["step"] = schema_number("Timeline snap step in seconds (default: 0.05).");
		Array loops;
		loops.push_back("none");
		loops.push_back("linear");
		loops.push_back("pingpong");
		props["loop_mode"] = schema_string("Loop mode (default: 'none').", loops);
		props["autoplay"] = schema_bool("Set this animation as AnimationPlayer autoplay (default: false).");
		Array req;
		req.push_back("player_path");
		req.push_back("animation");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_create";
		t.title = "Create Animation";
		t.description = "Create an Animation inside an AnimationPlayer (auto-creating the AnimationLibrary if needed).";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_create;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name to delete.");
		props["library"] = schema_string("AnimationLibrary name (default: '').");
		Array req;
		req.push_back("player_path");
		req.push_back("animation");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_delete";
		t.title = "Delete Animation";
		t.description = "Remove an animation from an AnimationPlayer.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_delete;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name.");
		props["library"] = schema_string("AnimationLibrary name (default: '').");
		props["track_path"] = schema_string("Target NodePath relative to AnimationPlayer root_node, e.g. 'Door:rotation_degrees' or 'Light:light_energy'.");
		Array types;
		types.push_back("value");
		types.push_back("position_3d");
		types.push_back("rotation_3d");
		types.push_back("scale_3d");
		types.push_back("method");
		types.push_back("bezier");
		types.push_back("audio");
		types.push_back("animation");
		props["track_type"] = schema_string("Track type (default: 'value').", types);
		props["interpolation"] = schema_string("Interpolation: 'nearest', 'linear', or 'cubic' (default: 'linear').");
		props["update_mode"] = schema_string("For value tracks: 'continuous', 'discrete', or 'capture'.");
		Array req;
		req.push_back("player_path");
		req.push_back("animation");
		req.push_back("track_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_add_track";
		t.title = "Add Animation Track";
		t.description = "Add a property, transform, method, audio, or sub-animation track to an Animation.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_add_track;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name.");
		props["library"] = schema_string("AnimationLibrary name (default: '').");
		props["track_index"] = schema_int("0-based index of the track.");
		props["time"] = schema_number("Keyframe time in seconds (when inserting a single key).");
		props["value"] = schema_any("Keyframe value (when inserting a single key).");
		props["transition"] = schema_number("Easing transition curve (default: 1.0).");
		props["keys"] = schema_array("Optional batch array of {time, value, transition} keyframes to insert at once.", schema_object("Keyframe"));
		Array req;
		req.push_back("player_path");
		req.push_back("animation");
		req.push_back("track_index");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_insert_key";
		t.title = "Insert Animation Keyframe(s)";
		t.description = "Insert one or multiple keyframes onto an animation track.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_insert_key;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name.");
		props["library"] = schema_string("AnimationLibrary name (default: '').");
		props["track_index"] = schema_int("0-based track index.");
		props["key_index"] = schema_int("0-based keyframe index to remove.");
		props["time"] = schema_number("Alternative: remove keyframe at exact time.");
		Array req;
		req.push_back("player_path");
		req.push_back("animation");
		req.push_back("track_index");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_remove_key";
		t.title = "Remove Animation Keyframe";
		t.description = "Remove a keyframe from an animation track by index or time.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_remove_key;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["player_path"] = schema_string("Node path to the AnimationPlayer.");
		props["animation"] = schema_string("Animation name.");
		props["library"] = schema_string("AnimationLibrary name (default: '').");
		Array req;
		req.push_back("player_path");
		req.push_back("animation");

		MinaMCPServer::ToolDefinition t;
		t.name = "animation_get_details";
		t.title = "Inspect Animation Details";
		t.description = "Inspect all tracks and keyframes inside an Animation.";
		t.category = "animation";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_animation_get_details;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		Array ltypes;
		ltypes.push_back("DirectionalLight3D");
		ltypes.push_back("OmniLight3D");
		ltypes.push_back("SpotLight3D");
		ltypes.push_back("PointLight2D");
		ltypes.push_back("DirectionalLight2D");
		props["light_type"] = schema_string("Type of light node (default: 'OmniLight3D').", ltypes);
		props["parent_path"] = schema_string("Parent node path (default: '.').");
		props["name"] = schema_string("Node name for the light.");
		props["position"] = schema_any("Light position [x,y,z] (or [x,y] for 2D).");
		props["rotation_degrees"] = schema_any("Light rotation in degrees [x,y,z].");
		props["color"] = schema_any("Light color ('#ffeedd' or [r,g,b]).");
		props["energy"] = schema_number("Light energy intensity.");
		props["range"] = schema_number("Omni/Spot light range in meters.");
		props["attenuation"] = schema_number("Light attenuation curve.");
		props["spot_angle"] = schema_number("SpotLight3D cone angle in degrees.");
		props["volumetric_fog_energy"] = schema_number("Volumetric fog light scatter energy.");
		props["shadow_enabled"] = schema_bool("Enable real-time shadows.");

		MinaMCPServer::ToolDefinition t;
		t.name = "lighting_add_light";
		t.title = "Add Light";
		t.description = "Add and configure a 3D or 2D light (DirectionalLight3D, OmniLight3D, SpotLight3D, PointLight2D, DirectionalLight2D) in the edited scene.";
		t.category = "lighting";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_lighting_add_light;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Optional existing WorldEnvironment path (auto-detected or auto-created if omitted).");
		props["background_mode"] = schema_string("'color', 'sky', or 'clear_color'.");
		props["background_color"] = schema_any("Background color ('#080a0e' or [r,g,b]).");
		props["ambient_light_color"] = schema_any("Ambient light color.");
		props["ambient_light_energy"] = schema_number("Ambient light energy.");
		props["tonemap_mode"] = schema_string("'linear', 'reinhard', 'filmic', 'aces', or 'agx'.");
		props["glow_enabled"] = schema_bool("Enable glow/bloom.");
		props["glow_intensity"] = schema_number("Glow intensity.");
		props["fog_enabled"] = schema_bool("Enable distance fog.");
		props["fog_color"] = schema_any("Fog light color.");
		props["fog_density"] = schema_number("Fog density.");
		props["volumetric_fog_enabled"] = schema_bool("Enable volumetric fog.");
		props["volumetric_fog_density"] = schema_number("Volumetric fog density.");
		props["volumetric_fog_albedo"] = schema_any("Volumetric fog albedo color.");
		props["ssao_enabled"] = schema_bool("Enable SSAO.");
		props["sdfgi_enabled"] = schema_bool("Enable SDFGI.");
		props["adjustment_enabled"] = schema_bool("Enable color grading adjustments.");
		props["adjustment_brightness"] = schema_number("Brightness multiplier.");
		props["adjustment_contrast"] = schema_number("Contrast multiplier.");
		props["adjustment_saturation"] = schema_number("Saturation multiplier.");
		props["properties"] = schema_object("Additional Environment properties to set.");

		MinaMCPServer::ToolDefinition t;
		t.name = "lighting_configure_environment";
		t.title = "Configure WorldEnvironment";
		t.description = "Create or update the scene's WorldEnvironment and Environment (sky, ambient light, tonemap, fog, volumetric fog, glow, SSAO, color grading).";
		t.category = "lighting";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_lighting_configure_environment;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["inherits"] = schema_string("Optional base class name (e.g. 'Node3D', 'Light3D', 'Resource', 'Material', 'Mesh').");
		props["search"] = schema_string("Optional case-insensitive substring filter.");
		props["instantiable_only"] = schema_bool("Return only instantiable classes (default: true).");
		props["limit"] = schema_int("Max classes to return (default: 200).");

		MinaMCPServer::ToolDefinition t;
		t.name = "class_list";
		t.title = "List Engine Classes";
		t.description = "Query ClassDB for available Mina/Godot Engine classes, including custom C++ module classes.";
		t.category = "introspection";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_class_list;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["class_name"] = schema_string("Engine class name to inspect (e.g. 'HorrorInteractable3D', 'SpotLight3D', 'StandardMaterial3D').");
		props["no_inheritance"] = schema_bool("Only list members declared directly on this class (default: true).");
		Array req;
		req.push_back("class_name");

		MinaMCPServer::ToolDefinition t;
		t.name = "class_get_info";
		t.title = "Inspect Engine Class";
		t.description = "Inspect a class's parent, properties, methods, and signals directly from ClassDB.";
		t.category = "introspection";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_class_get_info;
		p_server->register_tool(t);
	}
}

/**************************************************************************/
/*  mcp_tools_scene.cpp                                                   */
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
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/geometry_3d.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/2d/node_2d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/resources/3d/shape_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"

namespace {

bool _require_editor(Dictionary &r_err) {
	if (!EditorInterface::get_singleton() || !EditorNode::get_singleton()) {
		r_err = mcp::make_error("This tool requires an open project in the Mina Engine Editor (currently in Project Manager mode).", "editor_required");
		return false;
	}
	return true;
}

Node *_resolve_node(const String &p_node_path, String &r_error) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		r_error = "EditorInterface is not available.";
		return nullptr;
	}
	Node *root = ei->get_edited_scene_root();
	if (!root) {
		r_error = "No scene is currently open in the editor. Open or create a scene first.";
		return nullptr;
	}
	String trimmed = p_node_path.strip_edges();
	if (trimmed.is_empty() || trimmed == "." || trimmed == "/" || trimmed == root->get_name()) {
		return root;
	}
	if (trimmed.begins_with("/")) {
		trimmed = trimmed.substr(1);
	}
	String root_prefix = String(root->get_name()) + "/";
	if (trimmed.begins_with(root_prefix)) {
		trimmed = trimmed.substr(root_prefix.length());
	}
	Node *found = root->get_node_or_null(NodePath(trimmed));
	if (!found) {
		r_error = vformat("Node not found at path '%s' (scene root is '%s').", p_node_path, String(root->get_name()));
	}
	return found;
}

String _node_rel_path(Node *p_root, Node *p_node) {
	if (!p_root || !p_node || p_root == p_node) {
		return ".";
	}
	return String(p_root->get_path_to(p_node));
}

void _set_owner_recursive(Node *p_node, Node *p_owner) {
	if (!p_node) {
		return;
	}
	p_node->set_owner(p_owner);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_set_owner_recursive(p_node->get_child(i), p_owner);
	}
}

Dictionary _serialize_node_tree(Node *p_root, Node *p_node, int p_depth, int p_max_depth, bool p_include_transforms, int &r_visited, int p_max_nodes) {
	Dictionary d;
	r_visited++;
	d["name"] = String(p_node->get_name());
	d["path"] = _node_rel_path(p_root, p_node);
	d["class"] = p_node->get_class();

	if (!p_node->get_scene_file_path().is_empty() && p_node != p_root) {
		d["instance_scene"] = p_node->get_scene_file_path();
	}

	Ref<Script> scr = p_node->get_script();
	if (scr.is_valid() && !scr->get_path().is_empty()) {
		d["script"] = scr->get_path();
	}

	List<Node::GroupInfo> groups;
	p_node->get_groups(&groups);
	Array group_arr;
	for (const Node::GroupInfo &gi : groups) {
		if (gi.persistent) {
			group_arr.push_back(String(gi.name));
		}
	}
	if (!group_arr.is_empty()) {
		d["groups"] = group_arr;
	}

	if (p_include_transforms) {
		if (Node3D *n3 = Object::cast_to<Node3D>(p_node)) {
			d["position"] = mcp::variant_to_json(n3->get_position());
			d["rotation_degrees"] = mcp::variant_to_json(n3->get_rotation_degrees());
			d["scale"] = mcp::variant_to_json(n3->get_scale());
			d["visible"] = n3->is_visible();
		} else if (Node2D *n2 = Object::cast_to<Node2D>(p_node)) {
			d["position"] = mcp::variant_to_json(n2->get_position());
			d["rotation_degrees"] = n2->get_rotation_degrees();
			d["scale"] = mcp::variant_to_json(n2->get_scale());
			d["visible"] = n2->is_visible();
		} else if (Control *ctrl = Object::cast_to<Control>(p_node)) {
			d["position"] = mcp::variant_to_json(ctrl->get_position());
			d["size"] = mcp::variant_to_json(ctrl->get_size());
			d["visible"] = ctrl->is_visible();
		}
	}

	Array children;
	if (p_depth < p_max_depth) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			if (r_visited >= p_max_nodes) {
				d["truncated"] = true;
				break;
			}
			children.push_back(_serialize_node_tree(p_root, p_node->get_child(i), p_depth + 1, p_max_depth, p_include_transforms, r_visited, p_max_nodes));
		}
	} else if (p_node->get_child_count() > 0) {
		d["children_count"] = p_node->get_child_count();
	}
	if (!children.is_empty()) {
		d["children"] = children;
	}
	return d;
}

bool _apply_properties_to_object(Object *p_obj, const Dictionary &p_props, EditorUndoRedoManager *p_ur, Array &r_applied, Array &r_errors) {
	if (!p_obj || p_props.is_empty()) {
		return true;
	}
	Array keys = p_props.keys();
	for (int i = 0; i < keys.size(); i++) {
		String prop_name = String(keys[i]);
		Variant raw_val = p_props[prop_name];

		PropertyInfo pinfo;
		Variant::Type hint_type = Variant::NIL;
		bool found_prop = ClassDB::get_property_info(p_obj->get_class_name(), prop_name, &pinfo);
		if (found_prop) {
			hint_type = pinfo.type;
		} else {
			bool valid = false;
			Variant cur = p_obj->get(prop_name, &valid);
			if (valid) {
				hint_type = cur.get_type();
			}
		}

		String conv_err;
		Variant converted = mcp::json_to_variant(raw_val, hint_type, &conv_err);
		if (!conv_err.is_empty()) {
			r_errors.push_back(vformat("%s: %s", prop_name, conv_err));
			continue;
		}

		if (p_ur) {
			Variant old_val = p_obj->get(prop_name);
			p_ur->add_do_property(p_obj, prop_name, converted);
			p_ur->add_undo_property(p_obj, prop_name, old_val);
		} else {
			bool set_ok = false;
			p_obj->set(prop_name, converted, &set_ok);
			if (!set_ok) {
				r_errors.push_back(vformat("Property '%s' could not be set on %s.", prop_name, p_obj->get_class()));
				continue;
			}
		}
		r_applied.push_back(prop_name);
	}
	return r_errors.is_empty();
}

Dictionary _tool_scene_list_open(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = ei->get_edited_scene_root();
	Dictionary out;
	out["open_scenes"] = mcp::variant_to_json(ei->get_open_scenes());
	out["unsaved_scenes"] = mcp::variant_to_json(ei->get_unsaved_scenes());
	out["active_scene_path"] = root ? root->get_scene_file_path() : String();
	out["active_root_name"] = root ? String(root->get_name()) : String();
	return mcp::make_ok(out);
}

Dictionary _tool_scene_create(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String root_type = String(p_args.get("root_type", "Node3D")).strip_edges();
	String root_name = String(p_args.get("root_name", "")).strip_edges();
	String save_path = String(p_args.get("save_path", "")).strip_edges();

	if (!ClassDB::class_exists(root_type) || !ClassDB::is_parent_class(root_type, "Node") || !ClassDB::can_instantiate(root_type)) {
		return mcp::make_error(vformat("'%s' is not an instantiable Node class.", root_type), "invalid_class");
	}
	if (root_name.is_empty()) {
		root_name = root_type;
	}

	Node *root = Object::cast_to<Node>(ClassDB::instantiate(root_type));
	if (!root) {
		return mcp::make_error(vformat("Failed to instantiate '%s'.", root_type), "instantiate_failed");
	}
	root->set_name(root_name);

	EditorNode::get_singleton()->new_scene();
	EditorInterface::get_singleton()->add_root_node(root);

	String final_path;
	if (!save_path.is_empty()) {
		final_path = mcp::normalize_res_path(save_path);
		if (!mcp::is_safe_res_path(final_path)) {
			return mcp::make_error("Parameter 'save_path' must be a safe res:// path.", "invalid_path");
		}
		if (!final_path.ends_with(".tscn") && !final_path.ends_with(".scn")) {
			final_path += ".tscn";
		}
		String abs_dir = ProjectSettings::get_singleton()->globalize_path(final_path.get_base_dir());
		DirAccess::make_dir_recursive_absolute(abs_dir);
		EditorInterface::get_singleton()->save_scene_as(final_path, false);
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(final_path);
		}
	}

	Dictionary out;
	out["root_name"] = String(root->get_name());
	out["root_type"] = root->get_class();
	out["scene_path"] = final_path;
	return mcp::make_ok(out);
}

Dictionary _tool_scene_open(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// scene path.", "invalid_path");
	}
	if (!ResourceLoader::exists(path)) {
		return mcp::make_error(vformat("Scene file does not exist: '%s'.", path), "not_found");
	}
	EditorInterface::get_singleton()->open_scene_from_path(path);
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Dictionary out;
	out["path"] = path;
	out["root_name"] = root ? String(root->get_name()) : String();
	out["root_class"] = root ? root->get_class() : String();
	return mcp::make_ok(out);
}

Dictionary _tool_scene_save(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	bool save_all = bool(p_args.get("save_all", false));
	String save_as = String(p_args.get("save_as", "")).strip_edges();

	if (save_all) {
		EditorInterface::get_singleton()->save_all_scenes();
		Dictionary out;
		out["saved_all"] = true;
		return mcp::make_ok(out);
	}

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return mcp::make_error("No active scene to save.", "no_scene");
	}

	if (!save_as.is_empty()) {
		String path = mcp::normalize_res_path(save_as);
		if (!mcp::is_safe_res_path(path)) {
			return mcp::make_error("Parameter 'save_as' must be a valid res:// path.", "invalid_path");
		}
		String abs_dir = ProjectSettings::get_singleton()->globalize_path(path.get_base_dir());
		DirAccess::make_dir_recursive_absolute(abs_dir);
		EditorInterface::get_singleton()->save_scene_as(path, false);
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(path);
		}
		Dictionary out;
		out["path"] = path;
		return mcp::make_ok(out);
	}

	if (root->get_scene_file_path().is_empty()) {
		return mcp::make_error("Current scene has never been saved. Provide 'save_as' (e.g. 'res://scenes/main.tscn').", "unsaved_scene");
	}
	Error save_err = EditorInterface::get_singleton()->save_scene();
	if (save_err != OK) {
		return mcp::make_error(vformat("Failed to save scene (error %d).", (int)save_err), "save_failed");
	}
	Dictionary out;
	out["path"] = root->get_scene_file_path();
	return mcp::make_ok(out);
}

Dictionary _tool_scene_close(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	Error close_err = EditorInterface::get_singleton()->close_scene();
	if (close_err != OK) {
		return mcp::make_error(vformat("Failed to close scene (error %d).", (int)close_err), "close_failed");
	}
	return mcp::make_ok();
}

Dictionary _tool_scene_get_tree(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String start_path = String(p_args.get("node_path", "."));
	int max_depth = CLAMP(int(p_args.get("max_depth", 16)), 0, 64);
	int max_nodes = CLAMP(int(p_args.get("max_nodes", 500)), 1, 2000);
	bool include_transforms = bool(p_args.get("include_transforms", true));

	String resolve_err;
	Node *start_node = _resolve_node(start_path, resolve_err);
	if (!start_node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	int visited = 0;
	Dictionary tree = _serialize_node_tree(root, start_node, 0, max_depth, include_transforms, visited, max_nodes);

	Dictionary out;
	out["scene_path"] = root->get_scene_file_path();
	out["node_count"] = visited;
	out["tree"] = tree;
	return mcp::make_ok(out);
}

Dictionary _tool_scene_instantiate(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String scene_path = mcp::normalize_res_path(String(p_args.get("scene_path", "")));
	String parent_path = String(p_args.get("parent_path", "."));
	String custom_name = String(p_args.get("name", "")).strip_edges();

	if (!mcp::is_safe_res_path(scene_path) || !ResourceLoader::exists(scene_path)) {
		return mcp::make_error(vformat("PackedScene does not exist: '%s'.", scene_path), "not_found");
	}
	String resolve_err;
	Node *parent = _resolve_node(parent_path, resolve_err);
	if (!parent) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	Ref<PackedScene> packed = ResourceLoader::load(scene_path);
	if (packed.is_null()) {
		return mcp::make_error(vformat("Failed to load PackedScene '%s'.", scene_path), "load_failed");
	}
	Node *instance = packed->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	if (!instance) {
		return mcp::make_error(vformat("Failed to instantiate PackedScene '%s'.", scene_path), "instantiate_failed");
	}
	if (!custom_name.is_empty()) {
		instance->set_name(custom_name);
	}

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Instantiate %s", scene_path.get_file()));
		ur->add_do_method(parent, "add_child", instance, true);
		ur->add_do_method(instance, "set_owner", root);
		ur->add_do_reference(instance);
		ur->add_undo_method(parent, "remove_child", instance);
		ur->commit_action();
	} else {
		parent->add_child(instance, true);
		instance->set_owner(root);
	}

	if (p_args.has("position")) {
		if (Node3D *n3 = Object::cast_to<Node3D>(instance)) {
			Vector3 pos;
			if (mcp::parse_vector3(p_args["position"], pos)) {
				n3->set_position(pos);
			}
		} else if (Node2D *n2 = Object::cast_to<Node2D>(instance)) {
			Vector2 pos;
			if (mcp::parse_vector2(p_args["position"], pos)) {
				n2->set_position(pos);
			}
		}
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["name"] = String(instance->get_name());
	out["path"] = _node_rel_path(root, instance);
	out["class"] = instance->get_class();
	out["scene_path"] = scene_path;
	return mcp::make_ok(out);
}

Dictionary _tool_node_create(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String cls = String(p_args.get("type", "")).strip_edges();
	String parent_path = String(p_args.get("parent_path", "."));
	String node_name = String(p_args.get("name", "")).strip_edges();
	Dictionary props = p_args.get("properties", Dictionary());

	if (cls.is_empty() || !ClassDB::class_exists(cls) || !ClassDB::is_parent_class(cls, "Node") || !ClassDB::can_instantiate(cls)) {
		return mcp::make_error(vformat("'%s' is not an instantiable Node class.", cls), "invalid_class");
	}

	String resolve_err;
	Node *parent = _resolve_node(parent_path, resolve_err);
	if (!parent) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	Node *node = Object::cast_to<Node>(ClassDB::instantiate(cls));
	if (!node) {
		return mcp::make_error(vformat("Failed to instantiate Node '%s'.", cls), "instantiate_failed");
	}
	node->set_name(node_name.is_empty() ? cls : node_name);

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Create %s", String(node->get_name())));
		ur->add_do_method(parent, "add_child", node, true);
		ur->add_do_method(node, "set_owner", root);
		ur->add_do_reference(node);
		ur->add_undo_method(parent, "remove_child", node);
		ur->commit_action();
	} else {
		parent->add_child(node, true);
		node->set_owner(root);
	}

	// Convenience transform arguments.
	if (Node3D *n3 = Object::cast_to<Node3D>(node)) {
		Vector3 v;
		if (p_args.has("position") && mcp::parse_vector3(p_args["position"], v)) {
			n3->set_position(v);
		}
		if (p_args.has("rotation_degrees") && mcp::parse_vector3(p_args["rotation_degrees"], v)) {
			n3->set_rotation_degrees(v);
		}
		if (p_args.has("scale") && mcp::parse_vector3(p_args["scale"], v)) {
			n3->set_scale(v);
		}
	} else if (Node2D *n2 = Object::cast_to<Node2D>(node)) {
		Vector2 v;
		if (p_args.has("position") && mcp::parse_vector2(p_args["position"], v)) {
			n2->set_position(v);
		}
		if (p_args.has("rotation_degrees")) {
			n2->set_rotation_degrees(double(p_args["rotation_degrees"]));
		}
		if (p_args.has("scale") && mcp::parse_vector2(p_args["scale"], v)) {
			n2->set_scale(v);
		}
	}

	Array applied, prop_errors;
	if (!props.is_empty()) {
		_apply_properties_to_object(node, props, nullptr, applied, prop_errors);
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["name"] = String(node->get_name());
	out["path"] = _node_rel_path(root, node);
	out["class"] = node->get_class();
	if (!applied.is_empty()) {
		out["applied_properties"] = applied;
	}
	if (!prop_errors.is_empty()) {
		out["property_warnings"] = prop_errors;
	}
	return mcp::make_ok(out);
}

Dictionary _tool_node_delete(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", ""));
	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (node == root) {
		return mcp::make_error("Cannot delete the scene root node. Use scene_close or create a new scene instead.", "cannot_delete_root");
	}
	Node *parent = node->get_parent();
	String removed_path = _node_rel_path(root, node);
	int idx = node->get_index();

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur && parent) {
		ur->create_action(vformat("MCP: Delete %s", String(node->get_name())));
		ur->add_do_method(parent, "remove_child", node);
		ur->add_undo_method(parent, "add_child", node);
		ur->add_undo_method(parent, "move_child", node, idx);
		ur->add_undo_method(node, "set_owner", root);
		ur->add_undo_reference(node);
		ur->commit_action();
	} else if (parent) {
		parent->remove_child(node);
		memdelete(node);
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["deleted_path"] = removed_path;
	return mcp::make_ok(out);
}

Dictionary _tool_node_duplicate(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", ""));
	String new_name = String(p_args.get("new_name", "")).strip_edges();
	String parent_path = String(p_args.get("parent_path", "")).strip_edges();

	String resolve_err;
	Node *src = _resolve_node(node_path, resolve_err);
	if (!src) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (src == root) {
		return mcp::make_error("Cannot duplicate the scene root node directly.", "cannot_duplicate_root");
	}
	Node *parent = parent_path.is_empty() ? src->get_parent() : _resolve_node(parent_path, resolve_err);
	if (!parent) {
		return mcp::make_error(resolve_err, "parent_not_found");
	}

	Node *dup = src->duplicate();
	if (!dup) {
		return mcp::make_error("Failed to duplicate node.", "duplicate_failed");
	}
	if (!new_name.is_empty()) {
		dup->set_name(new_name);
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Duplicate %s", String(src->get_name())));
		ur->add_do_method(parent, "add_child", dup, true);
		ur->add_do_reference(dup);
		ur->add_undo_method(parent, "remove_child", dup);
		ur->commit_action();
	} else {
		parent->add_child(dup, true);
	}
	_set_owner_recursive(dup, root);
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["name"] = String(dup->get_name());
	out["path"] = _node_rel_path(root, dup);
	out["class"] = dup->get_class();
	return mcp::make_ok(out);
}

Dictionary _tool_node_reparent(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", ""));
	String new_parent_path = String(p_args.get("new_parent_path", "."));
	bool keep_global = bool(p_args.get("keep_global_transform", true));

	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (node == root) {
		return mcp::make_error("Cannot reparent the scene root node.", "invalid_operation");
	}
	Node *new_parent = _resolve_node(new_parent_path, resolve_err);
	if (!new_parent) {
		return mcp::make_error(resolve_err, "parent_not_found");
	}
	if (new_parent == node || node->is_ancestor_of(new_parent)) {
		return mcp::make_error("Cannot reparent a node to itself or one of its descendants.", "invalid_operation");
	}

	node->reparent(new_parent, keep_global);
	_set_owner_recursive(node, root);
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["new_path"] = _node_rel_path(root, node);
	return mcp::make_ok(out);
}

Dictionary _tool_node_rename(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", ""));
	String new_name = String(p_args.get("new_name", "")).strip_edges();
	if (new_name.is_empty()) {
		return mcp::make_error("Parameter 'new_name' is required.", "invalid_argument");
	}
	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	String old_name = node->get_name();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Rename %s to %s", old_name, new_name));
		ur->add_do_property(node, "name", new_name);
		ur->add_undo_property(node, "name", old_name);
		ur->commit_action();
	} else {
		node->set_name(new_name);
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["old_name"] = old_name;
	out["new_name"] = String(node->get_name());
	out["path"] = _node_rel_path(root, node);
	return mcp::make_ok(out);
}

Dictionary _tool_node_get_properties(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	String filter = String(p_args.get("filter", "")).strip_edges().to_lower();
	bool include_internal = bool(p_args.get("include_internal", false));

	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	Dictionary props_out;
	List<PropertyInfo> plist;
	node->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_SUBGROUP)) {
			continue;
		}
		if (!include_internal && !(pi.usage & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE))) {
			continue;
		}
		if (!filter.is_empty() && !pi.name.to_lower().contains(filter)) {
			continue;
		}
		Variant val = node->get(pi.name);
		Dictionary entry;
		entry["type"] = Variant::get_type_name(pi.type);
		entry["value"] = mcp::variant_to_json(val);
		props_out[pi.name] = entry;
	}

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Dictionary out;
	out["name"] = String(node->get_name());
	out["path"] = _node_rel_path(root, node);
	out["class"] = node->get_class();
	out["properties"] = props_out;
	return mcp::make_ok(out);
}

Dictionary _tool_node_set_properties(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	Dictionary props = p_args.get("properties", Dictionary());
	if (props.is_empty()) {
		return mcp::make_error("Parameter 'properties' must be a non-empty object.", "invalid_argument");
	}
	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	Array applied, prop_errors;
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Set properties on %s", String(node->get_name())));
		_apply_properties_to_object(node, props, ur, applied, prop_errors);
		ur->commit_action();
	} else {
		_apply_properties_to_object(node, props, nullptr, applied, prop_errors);
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Dictionary out;
	out["path"] = _node_rel_path(root, node);
	out["applied"] = applied;
	if (!prop_errors.is_empty()) {
		out["errors"] = prop_errors;
	}
	return mcp::make_ok(out);
}

Dictionary _tool_node_set_transform(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	bool global = bool(p_args.get("global", false));

	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (Node3D *n3 = Object::cast_to<Node3D>(node)) {
		if (ur) {
			ur->create_action(vformat("MCP: Transform %s", String(node->get_name())));
			ur->add_undo_property(n3, "transform", n3->get_transform());
		}
		Vector3 v;
		if (p_args.has("position") && mcp::parse_vector3(p_args["position"], v)) {
			if (global) {
				n3->set_global_position(v);
			} else {
				n3->set_position(v);
			}
		}
		if (p_args.has("rotation_degrees") && mcp::parse_vector3(p_args["rotation_degrees"], v)) {
			if (global) {
				n3->set_global_rotation_degrees(v);
			} else {
				n3->set_rotation_degrees(v);
			}
		}
		if (p_args.has("scale") && mcp::parse_vector3(p_args["scale"], v)) {
			n3->set_scale(v);
		}
		if (ur) {
			ur->add_do_property(n3, "transform", n3->get_transform());
			ur->commit_action(false);
		}
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
		Dictionary out;
		out["position"] = mcp::variant_to_json(n3->get_position());
		out["rotation_degrees"] = mcp::variant_to_json(n3->get_rotation_degrees());
		out["scale"] = mcp::variant_to_json(n3->get_scale());
		return mcp::make_ok(out);
	}

	if (Node2D *n2 = Object::cast_to<Node2D>(node)) {
		Vector2 v;
		if (p_args.has("position") && mcp::parse_vector2(p_args["position"], v)) {
			if (global) {
				n2->set_global_position(v);
			} else {
				n2->set_position(v);
			}
		}
		if (p_args.has("rotation_degrees")) {
			if (global) {
				n2->set_global_rotation_degrees(double(p_args["rotation_degrees"]));
			} else {
				n2->set_rotation_degrees(double(p_args["rotation_degrees"]));
			}
		}
		if (p_args.has("scale") && mcp::parse_vector2(p_args["scale"], v)) {
			n2->set_scale(v);
		}
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
		Dictionary out;
		out["position"] = mcp::variant_to_json(n2->get_position());
		out["rotation_degrees"] = n2->get_rotation_degrees();
		out["scale"] = mcp::variant_to_json(n2->get_scale());
		return mcp::make_ok(out);
	}

	if (Control *ctrl = Object::cast_to<Control>(node)) {
		Vector2 v;
		if (p_args.has("position") && mcp::parse_vector2(p_args["position"], v)) {
			ctrl->set_position(v);
		}
		if (p_args.has("size") && mcp::parse_vector2(p_args["size"], v)) {
			ctrl->set_size(v);
		}
		if (p_args.has("scale") && mcp::parse_vector2(p_args["scale"], v)) {
			ctrl->set_scale(v);
		}
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
		Dictionary out;
		out["position"] = mcp::variant_to_json(ctrl->get_position());
		out["size"] = mcp::variant_to_json(ctrl->get_size());
		return mcp::make_ok(out);
	}

	return mcp::make_error(vformat("Node '%s' (%s) is not a Node3D, Node2D, or Control.", node_path, node->get_class()), "unsupported_node_type");
}

Dictionary _tool_node_manage_groups(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	String op = String(p_args.get("operation", "list")).strip_edges().to_lower();
	String group = String(p_args.get("group", "")).strip_edges();

	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	if (op == "add") {
		if (group.is_empty()) {
			return mcp::make_error("Parameter 'group' is required.", "invalid_argument");
		}
		node->add_to_group(group, true);
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
	} else if (op == "remove") {
		if (group.is_empty()) {
			return mcp::make_error("Parameter 'group' is required.", "invalid_argument");
		}
		node->remove_from_group(group);
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
	} else if (op != "list") {
		return mcp::make_error("Parameter 'operation' must be 'list', 'add', or 'remove'.", "invalid_argument");
	}

	List<Node::GroupInfo> groups;
	node->get_groups(&groups);
	Array arr;
	for (const Node::GroupInfo &gi : groups) {
		if (gi.persistent) {
			arr.push_back(String(gi.name));
		}
	}
	Dictionary out;
	out["groups"] = arr;
	return mcp::make_ok(out);
}

Dictionary _tool_node_connect_signal(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String source_path = String(p_args.get("source_path", "."));
	String signal_name = String(p_args.get("signal", "")).strip_edges();
	String target_path = String(p_args.get("target_path", "."));
	String method_name = String(p_args.get("method", "")).strip_edges();

	if (signal_name.is_empty() || method_name.is_empty()) {
		return mcp::make_error("Parameters 'signal' and 'method' are required.", "invalid_argument");
	}
	String resolve_err;
	Node *src = _resolve_node(source_path, resolve_err);
	if (!src) {
		return mcp::make_error(resolve_err, "source_not_found");
	}
	Node *dst = _resolve_node(target_path, resolve_err);
	if (!dst) {
		return mcp::make_error(resolve_err, "target_not_found");
	}
	if (!src->has_signal(signal_name)) {
		return mcp::make_error(vformat("Node '%s' (%s) has no signal '%s'.", source_path, src->get_class(), signal_name), "signal_not_found");
	}

	Callable cb(dst, StringName(method_name));
	if (src->is_connected(signal_name, cb)) {
		Dictionary out;
		out["already_connected"] = true;
		return mcp::make_ok(out);
	}
	Error cerr = src->connect(signal_name, cb, Object::CONNECT_PERSIST);
	if (cerr != OK) {
		return mcp::make_error(vformat("Failed to connect signal '%s' (error %d).", signal_name, (int)cerr), "connect_failed");
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["source_path"] = source_path;
	out["signal"] = signal_name;
	out["target_path"] = target_path;
	out["method"] = method_name;
	return mcp::make_ok(out);
}

Dictionary _tool_node_disconnect_signal(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String source_path = String(p_args.get("source_path", "."));
	String signal_name = String(p_args.get("signal", "")).strip_edges();
	String target_path = String(p_args.get("target_path", "."));
	String method_name = String(p_args.get("method", "")).strip_edges();

	String resolve_err;
	Node *src = _resolve_node(source_path, resolve_err);
	if (!src) {
		return mcp::make_error(resolve_err, "source_not_found");
	}
	Node *dst = _resolve_node(target_path, resolve_err);
	if (!dst) {
		return mcp::make_error(resolve_err, "target_not_found");
	}

	Callable cb(dst, StringName(method_name));
	if (src->is_connected(signal_name, cb)) {
		src->disconnect(signal_name, cb);
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
	}
	return mcp::make_ok();
}

Dictionary _tool_node_list_signals(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	List<MethodInfo> sigs;
	node->get_signal_list(&sigs);
	Array signal_list;
	for (const MethodInfo &mi : sigs) {
		Dictionary sd;
		sd["name"] = mi.name;
		Array connections;
		List<Object::Connection> conns;
		node->get_signal_connection_list(mi.name, &conns);
		for (const Object::Connection &c : conns) {
			if (!(c.flags & Object::CONNECT_PERSIST)) {
				continue;
			}
			Dictionary cd;
			Node *target_node = Object::cast_to<Node>(c.callable.get_object());
			cd["target_path"] = target_node ? _node_rel_path(root, target_node) : String();
			cd["method"] = String(c.callable.get_method());
			connections.push_back(cd);
		}
		if (!connections.is_empty()) {
			sd["connections"] = connections;
		}
		signal_list.push_back(sd);
	}
	Dictionary out;
	out["signals"] = signal_list;
	return mcp::make_ok(out);
}

Dictionary _tool_node_call_method(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	String method = String(p_args.get("method", "")).strip_edges();
	Array raw_args = p_args.get("args", Array());
	if (method.is_empty()) {
		return mcp::make_error("Parameter 'method' is required.", "invalid_argument");
	}
	String resolve_err;
	Node *node = _resolve_node(node_path, resolve_err);
	if (!node) {
		return mcp::make_error(resolve_err, "node_not_found");
	}
	if (!node->has_method(method)) {
		return mcp::make_error(vformat("Node '%s' (%s) has no method '%s'.", node_path, node->get_class(), method), "method_not_found");
	}

	Vector<Variant> converted;
	Vector<const Variant *> argptrs;
	converted.resize(raw_args.size());
	argptrs.resize(raw_args.size());
	for (int i = 0; i < raw_args.size(); i++) {
		converted.write[i] = mcp::json_to_variant(raw_args[i]);
		argptrs.write[i] = &converted[i];
	}

	Callable::CallError ce;
	Variant ret = node->callp(StringName(method), argptrs.is_empty() ? nullptr : argptrs.ptrw(), (int)argptrs.size(), ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		return mcp::make_error(vformat("Method call '%s' failed with call error %d.", method, (int)ce.error), "call_failed");
	}
	Dictionary out;
	out["method"] = method;
	out["return_value"] = mcp::variant_to_json(ret);
	return mcp::make_ok(out);
}

Dictionary _tool_editor_select_nodes(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	EditorSelection *sel = EditorInterface::get_singleton()->get_selection();
	if (!sel) {
		return mcp::make_error("EditorSelection is not available.", "unavailable");
	}
	Array paths = p_args.get("node_paths", Array());
	sel->clear();
	Array selected;
	for (int i = 0; i < paths.size(); i++) {
		String rerr;
		Node *n = _resolve_node(String(paths[i]), rerr);
		if (n) {
			sel->add_node(n);
			selected.push_back(String(paths[i]));
		}
	}
	Dictionary out;
	out["selected"] = selected;
	return mcp::make_ok(out);
}

Dictionary _tool_editor_undo(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (!ur || !ur->has_undo()) {
		return mcp::make_error("Nothing to undo.", "empty_undo");
	}
	bool ok = ur->undo();
	Dictionary out;
	out["undone"] = ok;
	return mcp::make_ok(out);
}

Dictionary _tool_editor_redo(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (!ur || !ur->has_redo()) {
		return mcp::make_error("Nothing to redo.", "empty_redo");
	}
	bool ok = ur->redo();
	Dictionary out;
	out["redone"] = ok;
	return mcp::make_ok(out);
}

// ---------------- Enhanced Scene & Object Tools ----------------

void _find_nodes_recursive(
		Node *p_root,
		Node *p_node,
		const String &p_name_contains,
		const String &p_name_pattern,
		const String &p_class_name,
		bool p_include_subclasses,
		const String &p_group,
		bool p_filter_script,
		bool p_has_script_val,
		const String &p_prop_name,
		bool p_has_prop_val,
		const Variant &p_prop_val,
		int p_limit,
		Array &r_results,
		bool &r_truncated) {
	if (r_results.size() >= p_limit) {
		r_truncated = true;
		return;
	}

	bool match = true;
	String nname = p_node->get_name();
	if (!p_name_contains.is_empty() && !nname.to_lower().contains(p_name_contains)) {
		match = false;
	}
	if (match && !p_name_pattern.is_empty() && !nname.matchn(p_name_pattern)) {
		match = false;
	}
	if (match && !p_class_name.is_empty()) {
		String cls = p_node->get_class();
		if (p_include_subclasses) {
			if (cls != p_class_name && !ClassDB::is_parent_class(cls, p_class_name)) {
				match = false;
			}
		} else if (cls != p_class_name) {
			match = false;
		}
	}
	if (match && !p_group.is_empty() && !p_node->is_in_group(p_group)) {
		match = false;
	}
	if (match && p_filter_script) {
		Ref<Script> scr = p_node->get_script();
		if (scr.is_valid() != p_has_script_val) {
			match = false;
		}
	}
	if (match && !p_prop_name.is_empty()) {
		bool valid = false;
		Variant actual = p_node->get(p_prop_name, &valid);
		if (!valid) {
			match = false;
		} else if (p_has_prop_val) {
			Variant target = mcp::json_to_variant(p_prop_val, actual.get_type());
			if (actual != target && actual.stringify() != target.stringify()) {
				match = false;
			}
		}
	}

	if (match) {
		Dictionary entry;
		entry["name"] = nname;
		entry["path"] = _node_rel_path(p_root, p_node);
		entry["class"] = p_node->get_class();
		Ref<Script> scr = p_node->get_script();
		if (scr.is_valid() && !scr->get_path().is_empty()) {
			entry["script"] = scr->get_path();
		}
		if (Node3D *n3 = Object::cast_to<Node3D>(p_node)) {
			entry["position"] = mcp::variant_to_json(n3->get_position());
		} else if (Node2D *n2 = Object::cast_to<Node2D>(p_node)) {
			entry["position"] = mcp::variant_to_json(n2->get_position());
		}
		r_results.push_back(entry);
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (r_results.size() >= p_limit) {
			r_truncated = true;
			return;
		}
		_find_nodes_recursive(
				p_root, p_node->get_child(i),
				p_name_contains, p_name_pattern, p_class_name, p_include_subclasses,
				p_group, p_filter_script, p_has_script_val,
				p_prop_name, p_has_prop_val, p_prop_val,
				p_limit, r_results, r_truncated);
	}
}

Dictionary _tool_scene_find_nodes(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String root_path = String(p_args.get("root_path", "."));
	String rerr;
	Node *start_node = _resolve_node(root_path, rerr);
	if (!start_node) {
		return mcp::make_error(rerr, "node_not_found");
	}
	Node *scene_root = EditorInterface::get_singleton()->get_edited_scene_root();

	String name_contains = String(p_args.get("name_contains", "")).strip_edges().to_lower();
	String name_pattern = String(p_args.get("name_pattern", "")).strip_edges();
	String cls_name = String(p_args.get("class_name", "")).strip_edges();
	bool include_subclasses = bool(p_args.get("include_subclasses", true));
	String group = String(p_args.get("group", "")).strip_edges();
	bool filter_script = p_args.has("has_script");
	bool has_script_val = bool(p_args.get("has_script", false));
	String prop_name = String(p_args.get("property_name", "")).strip_edges();
	bool has_prop_val = p_args.has("property_value");
	Variant prop_val = p_args.get("property_value", Variant());
	int limit = CLAMP(int(p_args.get("limit", 100)), 1, 500);

	Array results;
	bool truncated = false;
	_find_nodes_recursive(
			scene_root, start_node,
			name_contains, name_pattern, cls_name, include_subclasses,
			group, filter_script, has_script_val,
			prop_name, has_prop_val, prop_val,
			limit, results, truncated);

	Dictionary out;
	out["count"] = results.size();
	out["truncated"] = truncated;
	out["nodes"] = results;
	return mcp::make_ok(out);
}

void _collect_node_map(Node *p_root, Node *p_node, HashMap<String, Node *> &r_map, Vector<String> &r_order) {
	String path = (p_root == p_node) ? String(".") : String(p_root->get_path_to(p_node));
	r_map[path] = p_node;
	r_order.push_back(path);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_node_map(p_root, p_node->get_child(i), r_map, r_order);
	}
}

Dictionary _tool_scene_diff(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	Node *cur_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!cur_root) {
		return mcp::make_error("No scene is currently open in the editor.", "no_scene");
	}
	String base_path = String(p_args.get("base_scene_path", "")).strip_edges();
	if (base_path.is_empty()) {
		base_path = cur_root->get_scene_file_path();
	} else {
		base_path = mcp::normalize_res_path(base_path);
	}

	Node *base_root = nullptr;
	if (!base_path.is_empty() && ResourceLoader::exists(base_path)) {
		Ref<PackedScene> packed = ResourceLoader::load(base_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE);
		if (packed.is_valid()) {
			base_root = packed->instantiate(PackedScene::GEN_EDIT_STATE_MAIN);
		}
	}

	HashMap<String, Node *> cur_map;
	Vector<String> cur_order;
	_collect_node_map(cur_root, cur_root, cur_map, cur_order);

	HashMap<String, Node *> base_map;
	Vector<String> base_order;
	if (base_root) {
		_collect_node_map(base_root, base_root, base_map, base_order);
	}

	Array added;
	Array removed;
	Array modified;

	for (int i = 0; i < cur_order.size(); i++) {
		const String &p = cur_order[i];
		Node *cn = cur_map[p];
		if (!base_map.has(p)) {
			Dictionary d;
			d["path"] = p;
			d["class"] = cn->get_class();
			added.push_back(d);
			continue;
		}
		Node *bn = base_map[p];
		Array changes;
		if (cn->get_class() != bn->get_class()) {
			Dictionary c;
			c["property"] = "_class";
			c["before"] = bn->get_class();
			c["after"] = cn->get_class();
			changes.push_back(c);
		}
		List<PropertyInfo> plist;
		cn->get_property_list(&plist);
		for (const PropertyInfo &pi : plist) {
			if (!(pi.usage & PROPERTY_USAGE_STORAGE)) {
				continue;
			}
			if (pi.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_SUBGROUP)) {
				continue;
			}
			if (pi.name == "owner" || pi.name == "scene_file_path") {
				continue;
			}
			Variant cv = cn->get(pi.name);
			Variant bv = bn->get(pi.name);
			Variant cj = mcp::variant_to_json(cv);
			Variant bj = mcp::variant_to_json(bv);
			if (cj != bj) {
				Dictionary c;
				c["property"] = pi.name;
				c["before"] = bj;
				c["after"] = cj;
				changes.push_back(c);
			}
		}
		if (!changes.is_empty()) {
			Dictionary m;
			m["path"] = p;
			m["class"] = cn->get_class();
			m["changes"] = changes;
			modified.push_back(m);
		}
	}

	for (int i = 0; i < base_order.size(); i++) {
		const String &p = base_order[i];
		if (!cur_map.has(p)) {
			Dictionary d;
			d["path"] = p;
			d["class"] = base_map[p]->get_class();
			removed.push_back(d);
		}
	}

	if (base_root) {
		memdelete(base_root);
	}

	Dictionary out;
	out["base_scene_path"] = base_path;
	out["has_saved_base"] = !base_path.is_empty() && !base_order.is_empty();
	out["has_changes"] = !added.is_empty() || !removed.is_empty() || !modified.is_empty();
	out["added_nodes"] = added;
	out["removed_nodes"] = removed;
	out["modified_nodes"] = modified;
	return mcp::make_ok(out);
}

Dictionary _tool_scene_create_inherited(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String base_path = mcp::normalize_res_path(String(p_args.get("base_scene_path", "")));
	String root_name = String(p_args.get("root_name", "")).strip_edges();
	String save_path = String(p_args.get("save_path", "")).strip_edges();

	if (!mcp::is_safe_res_path(base_path) || !ResourceLoader::exists(base_path)) {
		return mcp::make_error(vformat("Base scene does not exist: '%s'.", base_path), "not_found");
	}
	Ref<PackedScene> base_packed = ResourceLoader::load(base_path, "PackedScene");
	if (base_packed.is_null()) {
		return mcp::make_error(vformat("Failed to load base PackedScene '%s'.", base_path), "load_failed");
	}

	Node *inherited_root = base_packed->instantiate(PackedScene::GEN_EDIT_STATE_MAIN_INHERITED);
	if (!inherited_root) {
		return mcp::make_error("Failed to instantiate inherited scene.", "instantiate_failed");
	}
	if (!root_name.is_empty()) {
		inherited_root->set_name(root_name);
	}

	EditorNode::get_singleton()->new_scene();
	EditorInterface::get_singleton()->add_root_node(inherited_root);

	String final_path;
	if (!save_path.is_empty()) {
		final_path = mcp::normalize_res_path(save_path);
		if (!mcp::is_safe_res_path(final_path)) {
			return mcp::make_error("Parameter 'save_path' must be a valid res:// path.", "invalid_path");
		}
		if (!final_path.ends_with(".tscn") && !final_path.ends_with(".scn")) {
			final_path += ".tscn";
		}
		String abs_dir = ProjectSettings::get_singleton()->globalize_path(final_path.get_base_dir());
		DirAccess::make_dir_recursive_absolute(abs_dir);
		EditorInterface::get_singleton()->save_scene_as(final_path, false);
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(final_path);
		}
	}

	Dictionary out;
	out["base_scene_path"] = base_path;
	out["root_name"] = String(inherited_root->get_name());
	out["root_class"] = inherited_root->get_class();
	out["scene_path"] = final_path;
	return mcp::make_ok(out);
}

Dictionary _tool_scene_pack_selection(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String save_path = mcp::normalize_res_path(String(p_args.get("save_path", "")));
	if (!mcp::is_safe_res_path(save_path)) {
		return mcp::make_error("Parameter 'save_path' must be a valid res:// path (e.g. 'res://scenes/prop.tscn').", "invalid_path");
	}
	if (!save_path.ends_with(".tscn") && !save_path.ends_with(".scn")) {
		save_path += ".tscn";
	}

	Node *scene_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!scene_root) {
		return mcp::make_error("No scene is currently open.", "no_scene");
	}

	Vector<Node *> selected_nodes;
	if (p_args.has("node_paths") && p_args["node_paths"].get_type() == Variant::ARRAY) {
		Array paths = p_args["node_paths"];
		for (int i = 0; i < paths.size(); i++) {
			String rerr;
			Node *n = _resolve_node(String(paths[i]), rerr);
			if (!n) {
				return mcp::make_error(rerr, "node_not_found");
			}
			selected_nodes.push_back(n);
		}
	} else if (EditorInterface::get_singleton()->get_selection()) {
		TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_top_selected_nodes();
		for (int i = 0; i < sel.size(); i++) {
			Node *n = Object::cast_to<Node>(sel[i]);
			if (n) {
				selected_nodes.push_back(n);
			}
		}
	}

	if (selected_nodes.is_empty()) {
		return mcp::make_error("No nodes selected. Pass 'node_paths' or select nodes in the editor first.", "empty_selection");
	}

	String root_name = String(p_args.get("root_name", "")).strip_edges();
	String root_type = String(p_args.get("root_type", "Node3D")).strip_edges();
	bool replace_with_instance = bool(p_args.get("replace_with_instance", false));

	Node *pack_root = nullptr;
	if (selected_nodes.size() == 1) {
		pack_root = selected_nodes[0]->duplicate();
		if (!root_name.is_empty()) {
			pack_root->set_name(root_name);
		}
	} else {
		if (!ClassDB::class_exists(root_type) || !ClassDB::is_parent_class(root_type, "Node") || !ClassDB::can_instantiate(root_type)) {
			root_type = "Node3D";
		}
		pack_root = Object::cast_to<Node>(ClassDB::instantiate(root_type));
		pack_root->set_name(root_name.is_empty() ? save_path.get_file().get_basename() : root_name);
		for (int i = 0; i < selected_nodes.size(); i++) {
			Node *dup = selected_nodes[i]->duplicate();
			if (dup) {
				pack_root->add_child(dup, true);
			}
		}
	}

	for (int i = 0; i < pack_root->get_child_count(); i++) {
		_set_owner_recursive(pack_root->get_child(i), pack_root);
	}

	Ref<PackedScene> packed;
	packed.instantiate();
	Error perr = packed->pack(pack_root);
	String packed_root_name = pack_root->get_name();
	memdelete(pack_root);
	if (perr != OK) {
		return mcp::make_error(vformat("Failed to pack nodes into PackedScene (error %d).", (int)perr), "pack_failed");
	}

	String abs_dir = ProjectSettings::get_singleton()->globalize_path(save_path.get_base_dir());
	DirAccess::make_dir_recursive_absolute(abs_dir);
	Error serr = ResourceSaver::save(packed, save_path);
	if (serr != OK) {
		return mcp::make_error(vformat("Failed to save PackedScene to '%s' (error %d).", save_path, (int)serr), "save_failed");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(save_path);
	}

	String instance_path;
	if (replace_with_instance && selected_nodes[0] != scene_root && selected_nodes[0]->get_parent()) {
		Node *parent = selected_nodes[0]->get_parent();
		Ref<PackedScene> reloaded = ResourceLoader::load(save_path, "PackedScene", ResourceFormatLoader::CACHE_MODE_REPLACE);
		if (reloaded.is_valid()) {
			Node *inst = reloaded->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
			if (inst) {
				for (int i = 0; i < selected_nodes.size(); i++) {
					Node *orig = selected_nodes[i];
					if (orig && orig != scene_root && orig->get_parent()) {
						orig->get_parent()->remove_child(orig);
						memdelete(orig);
					}
				}
				parent->add_child(inst, true);
				inst->set_owner(scene_root);
				instance_path = _node_rel_path(scene_root, inst);
				EditorInterface::get_singleton()->mark_scene_as_unsaved();
			}
		}
	}

	Dictionary out;
	out["save_path"] = save_path;
	out["root_name"] = packed_root_name;
	out["packed_count"] = selected_nodes.size();
	if (!instance_path.is_empty()) {
		out["instance_path"] = instance_path;
	}
	return mcp::make_ok(out);
}

bool _compute_node3d_global_aabb(Node *p_node, bool p_include_children, AABB &r_aabb, bool &r_has_aabb) {
	if (!p_node) {
		return false;
	}
	if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node)) {
		AABB local = vi->get_aabb();
		if (local.size != Vector3()) {
			AABB world = vi->get_global_transform().xform(local);
			if (!r_has_aabb) {
				r_aabb = world;
				r_has_aabb = true;
			} else {
				r_aabb.merge_with(world);
			}
		}
	} else if (CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node)) {
		if (cs->get_shape().is_valid()) {
			Ref<Mesh> dbg = cs->get_shape()->get_debug_mesh();
			if (dbg.is_valid()) {
				AABB world = cs->get_global_transform().xform(dbg->get_aabb());
				if (!r_has_aabb) {
					r_aabb = world;
					r_has_aabb = true;
				} else {
					r_aabb.merge_with(world);
				}
			}
		}
	}
	if (p_include_children) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			_compute_node3d_global_aabb(p_node->get_child(i), true, r_aabb, r_has_aabb);
		}
	}
	return r_has_aabb;
}

Dictionary _serialize_aabb(const AABB &p_aabb) {
	Dictionary d;
	d["position"] = mcp::variant_to_json(p_aabb.position);
	d["size"] = mcp::variant_to_json(p_aabb.size);
	d["center"] = mcp::variant_to_json(p_aabb.get_center());
	d["min"] = mcp::variant_to_json(p_aabb.position);
	d["max"] = mcp::variant_to_json(p_aabb.get_end());
	return d;
}

Dictionary _tool_node_get_bounds(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", "."));
	bool include_children = bool(p_args.get("include_children", true));
	String rerr;
	Node *node = _resolve_node(node_path, rerr);
	if (!node) {
		return mcp::make_error(rerr, "node_not_found");
	}

	if (Node3D *n3 = Object::cast_to<Node3D>(node)) {
		AABB global_aabb;
		bool has = false;
		_compute_node3d_global_aabb(n3, include_children, global_aabb, has);
		AABB local_aabb = has ? n3->get_global_transform().affine_inverse().xform(global_aabb) : AABB();

		Dictionary out;
		out["space"] = "3d";
		out["has_bounds"] = has;
		out["global_bounds"] = _serialize_aabb(global_aabb);
		out["local_bounds"] = _serialize_aabb(local_aabb);
		return mcp::make_ok(out);
	}

	if (Control *ctrl = Object::cast_to<Control>(node)) {
		Rect2 gr = ctrl->get_global_rect();
		Rect2 lr(ctrl->get_position(), ctrl->get_size());
		Dictionary out;
		out["space"] = "2d";
		out["has_bounds"] = true;
		out["global_bounds"] = mcp::variant_to_json(gr);
		out["local_bounds"] = mcp::variant_to_json(lr);
		return mcp::make_ok(out);
	}

	if (Node2D *n2 = Object::cast_to<Node2D>(node)) {
		Rect2 r(n2->get_global_position(), Vector2());
		Dictionary out;
		out["space"] = "2d";
		out["has_bounds"] = true;
		out["global_bounds"] = mcp::variant_to_json(r);
		return mcp::make_ok(out);
	}

	return mcp::make_error(vformat("Node '%s' (%s) has no spatial bounds.", node_path, node->get_class()), "unsupported_node_type");
}

Dictionary _tool_node_align(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String axis_str = String(p_args.get("axis", "x")).strip_edges().to_lower();
	int axis = (axis_str == "y") ? 1 : ((axis_str == "z") ? 2 : 0);
	String mode = String(p_args.get("mode", "center")).strip_edges().to_lower();
	bool use_bounds = bool(p_args.get("use_bounds", false));
	bool has_spacing = p_args.has("spacing");
	double spacing = double(p_args.get("spacing", 0.0));

	Vector<Node3D *> nodes3d;
	if (p_args.has("node_paths") && p_args["node_paths"].get_type() == Variant::ARRAY) {
		Array paths = p_args["node_paths"];
		for (int i = 0; i < paths.size(); i++) {
			String rerr;
			Node *n = _resolve_node(String(paths[i]), rerr);
			if (!n) {
				return mcp::make_error(rerr, "node_not_found");
			}
			if (Node3D *n3 = Object::cast_to<Node3D>(n)) {
				nodes3d.push_back(n3);
			}
		}
	} else if (EditorInterface::get_singleton()->get_selection()) {
		TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
		for (int i = 0; i < sel.size(); i++) {
			if (Node3D *n3 = Object::cast_to<Node3D>(sel[i])) {
				nodes3d.push_back(n3);
			}
		}
	}

	if (nodes3d.size() < 2) {
		return mcp::make_error("node_align requires at least 2 Node3D objects (pass 'node_paths' or select nodes).", "invalid_argument");
	}

	Vector<double> anchors;
	anchors.resize(nodes3d.size());
	double min_v = 1e30, max_v = -1e30, sum_v = 0.0;
	for (int i = 0; i < nodes3d.size(); i++) {
		double val = nodes3d[i]->get_global_position()[axis];
		if (use_bounds) {
			AABB aabb;
			bool has = false;
			if (_compute_node3d_global_aabb(nodes3d[i], true, aabb, has)) {
				if (mode == "min") {
					val = aabb.position[axis];
				} else if (mode == "max") {
					val = aabb.get_end()[axis];
				} else {
					val = aabb.get_center()[axis];
				}
			}
		}
		anchors.write[i] = val;
		min_v = MIN(min_v, val);
		max_v = MAX(max_v, val);
		sum_v += val;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Align %d nodes (%s/%s)", nodes3d.size(), axis_str, mode));
	}

	Array updated;
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();

	if (mode == "distribute") {
		// Sort indices by current coordinate along axis.
		Vector<int> order;
		for (int i = 0; i < nodes3d.size(); i++) {
			order.push_back(i);
		}
		for (int i = 0; i < order.size(); i++) {
			for (int j = i + 1; j < order.size(); j++) {
				if (anchors[order[j]] < anchors[order[i]]) {
					SWAP(order.write[i], order.write[j]);
				}
			}
		}
		double start_v = anchors[order[0]];
		double step = has_spacing ? spacing : ((max_v - min_v) / double(nodes3d.size() - 1));
		for (int k = 0; k < order.size(); k++) {
			int idx = order[k];
			Node3D *n = nodes3d[idx];
			double target_v = start_v + step * double(k);
			double delta = target_v - anchors[idx];
			Vector3 gp = n->get_global_position();
			if (ur) {
				ur->add_undo_property(n, "global_position", gp);
			}
			gp[axis] += float(delta);
			n->set_global_position(gp);
			if (ur) {
				ur->add_do_property(n, "global_position", gp);
			}
			Dictionary item;
			item["path"] = _node_rel_path(root, n);
			item["position"] = mcp::variant_to_json(n->get_position());
			updated.push_back(item);
		}
	} else {
		double target_v = anchors[0];
		if (mode == "min") {
			target_v = min_v;
		} else if (mode == "max") {
			target_v = max_v;
		} else if (mode == "center") {
			target_v = sum_v / double(nodes3d.size());
		}
		for (int i = 0; i < nodes3d.size(); i++) {
			Node3D *n = nodes3d[i];
			double delta = target_v - anchors[i];
			Vector3 gp = n->get_global_position();
			if (ur) {
				ur->add_undo_property(n, "global_position", gp);
			}
			gp[axis] += float(delta);
			n->set_global_position(gp);
			if (ur) {
				ur->add_do_property(n, "global_position", gp);
			}
			Dictionary item;
			item["path"] = _node_rel_path(root, n);
			item["position"] = mcp::variant_to_json(n->get_position());
			updated.push_back(item);
		}
	}

	if (ur) {
		ur->commit_action(false);
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["axis"] = axis_str;
	out["mode"] = mode;
	out["updated"] = updated;
	return mcp::make_ok(out);
}

void _raycast_scene_surfaces(
		Node *p_node,
		Node *p_exclude_subtree,
		const Vector3 &p_origin,
		const Vector3 &p_dir,
		real_t p_max_dist,
		bool &r_hit,
		real_t &r_best_dist,
		Vector3 &r_best_point,
		Vector3 &r_best_normal,
		Node *&r_best_node) {
	if (!p_node || p_node == p_exclude_subtree) {
		return;
	}

	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			Transform3D xform = mi->get_global_transform();
			AABB world_aabb = xform.xform(mi->get_aabb());
			Vector3 seg_end = p_origin + p_dir * p_max_dist;
			if (world_aabb.intersects_segment(p_origin, seg_end)) {
				Vector<Face3> faces = mesh->get_faces();
				for (int i = 0; i < faces.size(); i++) {
					Vector3 a = xform.xform(faces[i].vertex[0]);
					Vector3 b = xform.xform(faces[i].vertex[1]);
					Vector3 c = xform.xform(faces[i].vertex[2]);
					Vector3 hit_pt;
					if (Geometry3D::ray_intersects_triangle(p_origin, p_dir, a, b, c, &hit_pt)) {
						real_t dist = p_origin.distance_to(hit_pt);
						if (dist <= p_max_dist && (!r_hit || dist < r_best_dist)) {
							Vector3 normal = (b - a).cross(c - a).normalized();
							if (normal.dot(p_dir) > 0) {
								normal = -normal;
							}
							r_hit = true;
							r_best_dist = dist;
							r_best_point = hit_pt;
							r_best_normal = normal;
							r_best_node = mi;
						}
					}
				}
			}
		}
	} else if (GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node)) {
		AABB local = gi->get_aabb();
		if (local.size != Vector3()) {
			Transform3D inv = gi->get_global_transform().affine_inverse();
			Vector3 l_from = inv.xform(p_origin);
			Vector3 l_to = inv.xform(p_origin + p_dir * p_max_dist);
			Vector3 l_clip, l_norm;
			if (!local.has_point(l_from) && local.intersects_segment(l_from, l_to, &l_clip, &l_norm)) {
				Vector3 w_pt = gi->get_global_transform().xform(l_clip);
				real_t dist = p_origin.distance_to(w_pt);
				if (dist <= p_max_dist && (!r_hit || dist < r_best_dist)) {
					Vector3 w_norm = gi->get_global_transform().basis.xform(l_norm).normalized();
					r_hit = true;
					r_best_dist = dist;
					r_best_point = w_pt;
					r_best_normal = w_norm;
					r_best_node = gi;
				}
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_raycast_scene_surfaces(p_node->get_child(i), p_exclude_subtree, p_origin, p_dir, p_max_dist, r_hit, r_best_dist, r_best_point, r_best_normal, r_best_node);
	}
}

Dictionary _tool_node_snap_to_surface(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor(err)) {
		return err;
	}
	String node_path = String(p_args.get("node_path", ""));
	String rerr;
	Node *node = _resolve_node(node_path, rerr);
	if (!node) {
		return mcp::make_error(rerr, "node_not_found");
	}
	Node3D *n3 = Object::cast_to<Node3D>(node);
	if (!n3) {
		return mcp::make_error(vformat("Node '%s' is not a Node3D.", node_path), "unsupported_node_type");
	}

	Vector3 dir(0, -1, 0);
	if (p_args.has("direction")) {
		mcp::parse_vector3(p_args["direction"], dir);
	}
	if (dir.length_squared() < 1e-6f) {
		return mcp::make_error("Parameter 'direction' must be a non-zero 3D vector.", "invalid_argument");
	}
	dir.normalize();

	real_t max_dist = real_t(MAX(0.1, double(p_args.get("max_distance", 100.0))));
	real_t offset = real_t(double(p_args.get("offset", 0.0)));
	bool align_to_bounds = bool(p_args.get("align_to_bounds", true));

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	Vector3 gp = n3->get_global_position();
	AABB obj_aabb;
	bool has_aabb = _compute_node3d_global_aabb(n3, true, obj_aabb, has_aabb);
	Vector3 ray_origin = has_aabb ? obj_aabb.get_center() : gp;

	bool hit = false;
	real_t best_dist = max_dist;
	Vector3 best_point;
	Vector3 best_normal = -dir;
	Node *best_node = nullptr;

	_raycast_scene_surfaces(root, n3, ray_origin, dir, max_dist, hit, best_dist, best_point, best_normal, best_node);
	if (!hit) {
		return mcp::make_error("No surface found in the given direction within max_distance.", "no_surface_hit");
	}

	// Calculate shift along dir so object sits flush on best_point (+ offset away from surface).
	real_t extent_in_dir = 0.0f;
	if (align_to_bounds && has_aabb) {
		// Support point of obj_aabb in direction `dir` relative to ray_origin (aabb center).
		Vector3 half = obj_aabb.size * 0.5f;
		extent_in_dir = Math::abs(dir.x) * half.x + Math::abs(dir.y) * half.y + Math::abs(dir.z) * half.z;
	}
	real_t shift_dist = best_dist - extent_in_dir - offset;
	Vector3 new_gp = gp + dir * shift_dist;

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	if (ur) {
		ur->create_action(vformat("MCP: Snap %s to surface", String(n3->get_name())));
		ur->add_undo_property(n3, "global_position", gp);
		n3->set_global_position(new_gp);
		ur->add_do_property(n3, "global_position", new_gp);
		ur->commit_action(false);
	} else {
		n3->set_global_position(new_gp);
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["node_path"] = _node_rel_path(root, n3);
	out["surface_node"] = best_node ? _node_rel_path(root, best_node) : String();
	out["hit_point"] = mcp::variant_to_json(best_point);
	out["hit_normal"] = mcp::variant_to_json(best_normal);
	out["new_position"] = mcp::variant_to_json(n3->get_position());
	out["new_global_position"] = mcp::variant_to_json(new_gp);
	return mcp::make_ok(out);
}

} // namespace

void mcp_register_scene_tools(MinaMCPServer *p_server) {
	using namespace mcp;

	{
		MinaMCPServer::ToolDefinition t;
		t.name = "scene_list_open";
		t.title = "List Open Scenes";
		t.description = "List currently open scenes, unsaved scenes, and the active edited scene root.";
		t.category = "scene";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_scene_list_open;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["root_type"] = schema_string("Godot/Mina Node class for the scene root (default: 'Node3D'). Examples: 'Node3D', 'Node2D', 'Control', 'CharacterBody3D'.");
		props["root_name"] = schema_string("Name of the root node.");
		props["save_path"] = schema_string("Optional res:// path to immediately save the new scene, e.g. 'res://scenes/level_1.tscn'.");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_create";
		t.title = "Create Scene";
		t.description = "Create a new scene with the specified root node type in the editor and optionally save it to a .tscn file.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_scene_create;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Scene path under res://, e.g. 'res://scenes/main.tscn'.");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_open";
		t.title = "Open Scene";
		t.description = "Open an existing .tscn or .scn scene in the editor.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_scene_open;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["save_as"] = schema_string("Optional res:// path to save the current scene as.");
		props["save_all"] = schema_bool("If true, save all open scenes (default: false).");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_save";
		t.title = "Save Scene";
		t.description = "Save the active edited scene (or save as a new path, or save all open scenes).";
		t.category = "scene";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_scene_save;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "scene_close";
		t.title = "Close Scene";
		t.description = "Close the currently active scene tab in the editor.";
		t.category = "scene";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_scene_close;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Starting node path relative to scene root (default: '.').");
		props["max_depth"] = schema_int("Maximum tree depth to traverse (default: 16).");
		props["max_nodes"] = schema_int("Maximum total nodes to include (default: 500).");
		props["include_transforms"] = schema_bool("Include position/rotation/scale/visibility (default: true).");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_get_tree";
		t.title = "Get Scene Tree";
		t.description = "Inspect the node hierarchy of the currently edited scene, including classes, paths, scripts, groups, and transforms.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_scene_get_tree;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["scene_path"] = schema_string("res:// path to the PackedScene (.tscn/.scn/.glb) to instantiate.");
		props["parent_path"] = schema_string("Parent node path relative to edited scene root (default: '.').");
		props["name"] = schema_string("Optional custom node name for the instance.");
		props["position"] = schema_any("Optional initial position ([x,y,z] or {x,y,z} for 3D, [x,y] for 2D).");
		Array req;
		req.push_back("scene_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_instantiate";
		t.title = "Instantiate Sub-Scene";
		t.description = "Instantiate a PackedScene (.tscn/.scn/.glb) as a child of a node in the currently edited scene.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_scene_instantiate;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["type"] = schema_string("Node class name (e.g. 'MeshInstance3D', 'StaticBody3D', 'CollisionShape3D', 'Camera3D', 'CharacterBody3D', 'AudioStreamPlayer3D', 'HorrorInteractable3D', 'Label').");
		props["parent_path"] = schema_string("Parent node path relative to edited scene root (default: '.').");
		props["name"] = schema_string("Node name.");
		props["position"] = schema_any("Optional initial position ([x,y,z] or {x,y,z} for 3D; [x,y] for 2D).");
		props["rotation_degrees"] = schema_any("Optional initial rotation in degrees.");
		props["scale"] = schema_any("Optional initial scale.");
		props["properties"] = schema_object("Optional initial properties map. Supports inline sub-resources like {\"mesh\": {\"class\": \"BoxMesh\", \"size\": [2, 2, 2]}} and res:// paths.");
		Array req;
		req.push_back("type");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_create";
		t.title = "Create Node";
		t.description = "Instantiate any Godot or Mina Engine Node class into the edited scene with UndoRedo support, optional transform, and initial properties.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_create;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Path of the node to delete relative to scene root.");
		Array req;
		req.push_back("node_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_delete";
		t.title = "Delete Node";
		t.description = "Delete a node and its children from the currently edited scene (with UndoRedo support).";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_delete;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Path of the node to duplicate.");
		props["new_name"] = schema_string("Optional name for the duplicated node.");
		props["parent_path"] = schema_string("Optional parent path for the duplicate (defaults to same parent).");
		Array req;
		req.push_back("node_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_duplicate";
		t.title = "Duplicate Node";
		t.description = "Duplicate an existing node (and its subtree) in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_duplicate;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Path of the node to move.");
		props["new_parent_path"] = schema_string("Path of the new parent node (default: '.').");
		props["keep_global_transform"] = schema_bool("Preserve world-space transform after reparenting (default: true).");
		Array req;
		req.push_back("node_path");
		req.push_back("new_parent_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_reparent";
		t.title = "Reparent Node";
		t.description = "Move a node under a different parent node in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_reparent;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Path of the node to rename.");
		props["new_name"] = schema_string("New node name.");
		Array req;
		req.push_back("node_path");
		req.push_back("new_name");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_rename";
		t.title = "Rename Node";
		t.description = "Rename a node in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_rename;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root (default: '.').");
		props["filter"] = schema_string("Optional case-insensitive substring filter for property names.");
		props["include_internal"] = schema_bool("Include non-editor internal properties (default: false).");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_get_properties";
		t.title = "Get Node Properties";
		t.description = "Read properties and current values of a node in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_node_get_properties;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root (default: '.').");
		props["properties"] = schema_object("Map of property_name -> value. Supports Vector2/3, Color ('#ff0000' or [r,g,b,a]), res:// resource paths, and inline sub-resources {\"class\": \"BoxShape3D\", \"size\": [1,2,1]}.");
		Array req;
		req.push_back("node_path");
		req.push_back("properties");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_set_properties";
		t.title = "Set Node Properties";
		t.description = "Set one or more properties on a node directly via Godot's Object::set API with UndoRedo support.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_set_properties;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root.");
		props["position"] = schema_any("Position: [x,y,z] or {x,y,z} for Node3D; [x,y] for Node2D/Control.");
		props["rotation_degrees"] = schema_any("Rotation in degrees: [x,y,z] for Node3D; number for Node2D.");
		props["scale"] = schema_any("Scale: [x,y,z] for Node3D; [x,y] for Node2D/Control.");
		props["size"] = schema_any("Size [w,h] (for Control nodes).");
		props["global"] = schema_bool("Apply position/rotation in global world space (default: false).");
		Array req;
		req.push_back("node_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_set_transform";
		t.title = "Set Node Transform";
		t.description = "Set position, rotation (degrees), scale, or size on a Node3D, Node2D, or Control.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_set_transform;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root.");
		Array ops;
		ops.push_back("list");
		ops.push_back("add");
		ops.push_back("remove");
		props["operation"] = schema_string("'list', 'add', or 'remove' (default: 'list').", ops);
		props["group"] = schema_string("Group name (required for 'add' and 'remove').");
		Array req;
		req.push_back("node_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_manage_groups";
		t.title = "Manage Node Groups";
		t.description = "List, add, or remove persistent scene groups on a node.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_manage_groups;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["source_path"] = schema_string("Path of the node emitting the signal.");
		props["signal"] = schema_string("Signal name (e.g. 'body_entered', 'pressed', 'interacted').");
		props["target_path"] = schema_string("Path of the node receiving the signal.");
		props["method"] = schema_string("Method name on the target node's script.");
		Array req;
		req.push_back("source_path");
		req.push_back("signal");
		req.push_back("target_path");
		req.push_back("method");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_connect_signal";
		t.title = "Connect Signal";
		t.description = "Persistently connect a signal from one node to a method on another node in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_connect_signal;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["source_path"] = schema_string("Source node path.");
		props["signal"] = schema_string("Signal name.");
		props["target_path"] = schema_string("Target node path.");
		props["method"] = schema_string("Target method name.");
		Array req;
		req.push_back("source_path");
		req.push_back("signal");
		req.push_back("target_path");
		req.push_back("method");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_disconnect_signal";
		t.title = "Disconnect Signal";
		t.description = "Disconnect a signal connection between two nodes in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_disconnect_signal;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root (default: '.').");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_list_signals";
		t.title = "List Node Signals";
		t.description = "List all signals defined on a node and their persistent connections.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_node_list_signals;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root.");
		props["method"] = schema_string("Bound method name to invoke on the node.");
		props["args"] = schema_array("Optional positional arguments.");
		Array req;
		req.push_back("node_path");
		req.push_back("method");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_call_method";
		t.title = "Call Node Method";
		t.description = "Invoke any bound engine or tool-script method directly on a node in the edited scene.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_call_method;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_paths"] = schema_array("Node paths to select in the editor.", schema_string("Node path"));
		Array req;
		req.push_back("node_paths");

		MinaMCPServer::ToolDefinition t;
		t.name = "editor_select_nodes";
		t.title = "Select Nodes in Editor";
		t.description = "Set the current node selection in the SceneTree dock and Inspector.";
		t.category = "editor";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_editor_select_nodes;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "editor_undo";
		t.title = "Undo";
		t.description = "Undo the last action in the editor UndoRedo history.";
		t.category = "editor";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_editor_undo;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "editor_redo";
		t.title = "Redo";
		t.description = "Redo the previously undone action in the editor UndoRedo history.";
		t.category = "editor";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_editor_redo;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["root_path"] = schema_string("Subtree root to search within (default: '.').");
		props["name_contains"] = schema_string("Case-insensitive substring match against node name.");
		props["name_pattern"] = schema_string("Case-insensitive glob pattern against node name (e.g. 'Door*', '*Light*').");
		props["class_name"] = schema_string("Match node class (e.g. 'Light3D', 'MeshInstance3D', 'HorrorInteractable3D').");
		props["include_subclasses"] = schema_bool("Include subclasses of class_name (default: true).");
		props["group"] = schema_string("Only return nodes belonging to this scene group.");
		props["has_script"] = schema_bool("Filter by whether a script is attached.");
		props["property_name"] = schema_string("Only return nodes having this property (and matching property_value if supplied).");
		props["property_value"] = schema_any("Expected value for property_name.");
		props["limit"] = schema_int("Maximum matching nodes to return (default: 100, max: 500).");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_find_nodes";
		t.title = "Find Nodes in Scene";
		t.description = "Search nodes in the edited scene by name, class, group, script, or property without dumping the entire scene tree.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_scene_find_nodes;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["base_scene_path"] = schema_string("Optional res:// scene path to diff against (defaults to current scene's saved .tscn on disk).");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_diff";
		t.title = "Diff Scene Changes";
		t.description = "Compare the currently edited scene against its saved .tscn (or another scene), listing added, removed, and modified nodes/properties.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_scene_diff;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["base_scene_path"] = schema_string("res:// path of the base scene to inherit from.");
		props["root_name"] = schema_string("Optional new name for the inherited scene root.");
		props["save_path"] = schema_string("Optional res:// path to immediately save the inherited scene.");
		Array req;
		req.push_back("base_scene_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_create_inherited";
		t.title = "Create Inherited Scene";
		t.description = "Create a new scene inheriting from an existing PackedScene (.tscn) and optionally save it.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_scene_create_inherited;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["save_path"] = schema_string("Destination res:// .tscn path for the packed reusable scene.");
		props["node_paths"] = schema_array("Optional node paths to pack (defaults to current EditorSelection if omitted).", schema_string("Node path"));
		props["root_name"] = schema_string("Optional root node name for the packed scene.");
		props["root_type"] = schema_string("Wrapper root class when packing multiple nodes (default: 'Node3D').");
		props["replace_with_instance"] = schema_bool("Replace original node(s) in the current scene with an instance of the newly packed scene (default: false).");
		Array req;
		req.push_back("save_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "scene_pack_selection";
		t.title = "Pack Nodes into Reusable Scene";
		t.description = "Convert selected or specified nodes into a reusable .tscn PackedScene, optionally replacing them in-place with an instance.";
		t.category = "scene";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_scene_pack_selection;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path relative to scene root (default: '.').");
		props["include_children"] = schema_bool("Merge descendant VisualInstance3D / CollisionShape3D bounds (default: true).");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_get_bounds";
		t.title = "Get Node Bounding Box";
		t.description = "Compute local and global bounding box (3D AABB or 2D Rect2: position, size, center, min, max) for a node and its children.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_node_get_bounds;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_paths"] = schema_array("Node paths to align/distribute (defaults to current EditorSelection if omitted).", schema_string("Node path"));
		Array axes;
		axes.push_back("x");
		axes.push_back("y");
		axes.push_back("z");
		props["axis"] = schema_string("Axis along which to align or distribute (default: 'x').", axes);
		Array modes;
		modes.push_back("min");
		modes.push_back("center");
		modes.push_back("max");
		modes.push_back("first");
		modes.push_back("distribute");
		props["mode"] = schema_string("Alignment mode: 'min', 'center', 'max', 'first', or 'distribute' (default: 'center').", modes);
		props["spacing"] = schema_number("Optional fixed spacing between nodes when mode='distribute'.");
		props["use_bounds"] = schema_bool("Align using bounding box min/center/max instead of node pivot (default: false).");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_align";
		t.title = "Align / Distribute Nodes";
		t.description = "Align or evenly distribute multiple 3D nodes along X, Y, or Z axis with UndoRedo support.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_node_align;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("3D node path to snap.");
		props["direction"] = schema_any("Raycast direction vector (default: [0, -1, 0] downward onto floor; e.g. [1, 0, 0] for a wall).");
		props["max_distance"] = schema_number("Maximum raycast distance (default: 100.0).");
		props["offset"] = schema_number("Extra clearance offset away from surface (default: 0.0).");
		props["align_to_bounds"] = schema_bool("Place the node's bounding box flush against the surface rather than its pivot (default: true).");
		Array req;
		req.push_back("node_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "node_snap_to_surface";
		t.title = "Snap Node to Surface";
		t.description = "Snap a 3D node onto the nearest floor, wall, or ceiling mesh surface in the scene with UndoRedo support.";
		t.category = "nodes";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_node_snap_to_surface;
		p_server->register_tool(t);
	}

	// Register live scene resource.
	{
		MinaMCPServer::ResourceDefinition r;
		r.uri = "mina://scene/current";
		r.name = "Current Edited Scene Tree";
		r.description = "Live node tree of the currently edited scene in Mina Engine.";
		r.mime_type = "application/json";
		r.reader = []() -> Dictionary {
			Dictionary args;
			args["max_depth"] = 12;
			args["max_nodes"] = 300;
			return _tool_scene_get_tree(args);
		};
		p_server->register_resource(r);
	}
}

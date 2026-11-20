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
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/2d/node_2d.h"
#include "scene/3d/node_3d.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
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

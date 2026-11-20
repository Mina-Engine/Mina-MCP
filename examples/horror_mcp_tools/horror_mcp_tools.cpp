/**************************************************************************/
/*  horror_mcp_tools.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "horror_mcp_tools.h"

#ifdef MODULE_MCP_ENABLED

#include "../horror_interactable_3d.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "horror_director_editor_plugin.h"
#include "modules/mcp/mcp_server.h"
#include "modules/mcp/mcp_variant_codec.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"

namespace {

Dictionary _tool_horror_get_director_state(const Dictionary &p_args) {
	HorrorDirectorEditorPlugin *director = HorrorDirectorEditorPlugin::get_singleton();
	if (!director) {
		return mcp::make_error("HorrorDirectorEditorPlugin is not active.", "unavailable");
	}
	return mcp::make_ok(director->get_state_dictionary());
}

Dictionary _tool_horror_set_director_state(const Dictionary &p_args) {
	HorrorDirectorEditorPlugin *director = HorrorDirectorEditorPlugin::get_singleton();
	if (!director) {
		return mcp::make_error("HorrorDirectorEditorPlugin is not active.", "unavailable");
	}
	if (p_args.has("profile_index")) {
		director->set_profile_index(int(p_args["profile_index"]));
	}
	if (p_args.has("tension")) {
		director->set_tension(double(p_args["tension"]));
	}
	return mcp::make_ok(director->get_state_dictionary());
}

Dictionary _tool_horror_create_interactable(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) {
		return mcp::make_error("EditorInterface is not available.", "editor_required");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return mcp::make_error("No scene is currently open.", "no_scene");
	}
	String parent_path = String(p_args.get("parent_path", ".")).strip_edges();
	Node *parent = (parent_path.is_empty() || parent_path == "." || parent_path == "/") ? root : root->get_node_or_null(NodePath(parent_path));
	if (!parent) {
		return mcp::make_error(vformat("Parent node '%s' not found.", parent_path), "node_not_found");
	}

	String name = String(p_args.get("name", "Interactable")).strip_edges();
	String prompt = String(p_args.get("prompt", "Interact"));
	bool enabled = bool(p_args.get("enabled", true));
	bool add_collider = bool(p_args.get("add_collider", true));

	HorrorInteractable3D *interactable = memnew(HorrorInteractable3D);
	interactable->set_name(name.is_empty() ? String("Interactable") : name);
	interactable->set_interaction_prompt(prompt);
	interactable->set_interaction_enabled(enabled);

	Vector3 pos;
	if (p_args.has("position") && mcp::parse_vector3(p_args["position"], pos)) {
		interactable->set_position(pos);
	}

	parent->add_child(interactable, true);
	interactable->set_owner(root);

	if (add_collider) {
		StaticBody3D *body = memnew(StaticBody3D);
		body->set_name("InteractionBody");
		interactable->add_child(body, true);
		body->set_owner(root);

		CollisionShape3D *col = memnew(CollisionShape3D);
		col->set_name("CollisionShape3D");
		Ref<BoxShape3D> box;
		box.instantiate();
		Vector3 sz(1.0, 1.0, 1.0);
		if (p_args.has("collider_size")) {
			mcp::parse_vector3(p_args["collider_size"], sz);
		}
		box->set_size(sz);
		col->set_shape(box);
		body->add_child(col, true);
		col->set_owner(root);
	}

	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	Dictionary out;
	out["name"] = String(interactable->get_name());
	out["path"] = String(root->get_path_to(interactable));
	out["prompt"] = prompt;
	out["enabled"] = enabled;
	out["has_collider"] = add_collider;
	return mcp::make_ok(out);
}

} // namespace

void horror_register_mcp_tools(MinaMCPServer *p_server) {
	if (!p_server) {
		return;
	}
	using namespace mcp;

	{
		MinaMCPServer::ToolDefinition t;
		t.name = "horror_get_director_state";
		t.title = "Get Horror Director State";
		t.description = "Get the current Mina Engine Horror Director profile, tension level (0-100), and recommended scare/event candidates.";
		t.category = "horror";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_horror_get_director_state;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["profile_index"] = schema_int("Horror direction profile index (0: Psychological, 1: Survival, 2: Surveillance/Anomaly, 3: Exploration/Chase).");
		props["tension"] = schema_number("Tension level from 0 to 100.");

		MinaMCPServer::ToolDefinition t;
		t.name = "horror_set_director_state";
		t.title = "Set Horror Director State";
		t.description = "Update Mina Engine's Horror Director profile and tension level and retrieve updated event candidates.";
		t.category = "horror";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_horror_set_director_state;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["parent_path"] = schema_string("Parent node path in edited scene (default: '.').");
		props["name"] = schema_string("Node name (default: 'Interactable').");
		props["prompt"] = schema_string("Interaction prompt text shown to the player.");
		props["enabled"] = schema_bool("Whether interaction is initially enabled (default: true).");
		props["position"] = schema_any("3D position [x,y,z].");
		props["add_collider"] = schema_bool("Create child StaticBody3D + BoxShape3D collider for HorrorInteractor3D raycast (default: true).");
		props["collider_size"] = schema_any("BoxShape3D size [x,y,z] (default: [1,1,1]).");

		MinaMCPServer::ToolDefinition t;
		t.name = "horror_create_interactable";
		t.title = "Create Horror Interactable";
		t.description = "Create a HorrorInteractable3D node with prompt and optional raycast collider ready for HorrorInteractor3D.";
		t.category = "horror";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_horror_create_interactable;
		p_server->register_tool(t);
	}
}

#endif // MODULE_MCP_ENABLED

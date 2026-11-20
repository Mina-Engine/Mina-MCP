/**************************************************************************/
/*  mcp_tools_project.cpp                                                 */
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
#include "core/input/input_event.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_icons.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

namespace {

Dictionary _build_engine_status() {
	Dictionary d;
	d["engine"] = "Mina Engine";
	d["version"] = VERSION_FULL_CONFIG;
	d["os"] = OS::get_singleton()->get_name();
	d["executable_path"] = OS::get_singleton()->get_executable_path();
	if (MinaMCPServer::get_singleton()) {
		d["mcp"] = MinaMCPServer::get_singleton()->get_status_dictionary();
	}

	EditorNode *en = EditorNode::get_singleton();
	EditorInterface *ei = EditorInterface::get_singleton();
	d["in_editor"] = (en != nullptr && ei != nullptr);

	if (ProjectSettings::get_singleton()) {
		String res_path = ProjectSettings::get_singleton()->get_resource_path();
		d["project_path"] = res_path;
		if (!res_path.is_empty()) {
			d["project_name"] = String(GLOBAL_GET("application/config/name"));
			d["main_scene"] = String(GLOBAL_GET("application/run/main_scene"));
			d["renderer"] = String(GLOBAL_GET("rendering/renderer/rendering_method"));
		}
	}

	if (ei) {
		Node *root = ei->get_edited_scene_root();
		d["edited_scene_path"] = root ? root->get_scene_file_path() : String();
		d["edited_scene_root_name"] = root ? String(root->get_name()) : String();
		d["edited_scene_root_class"] = root ? root->get_class() : String();
		d["open_scenes"] = mcp::variant_to_json(ei->get_open_scenes());
		d["unsaved_scenes"] = mcp::variant_to_json(ei->get_unsaved_scenes());
		d["is_playing"] = ei->is_playing_scene();
		d["playing_scene"] = ei->get_playing_scene();
	}
	if (EditorUndoRedoManager::get_singleton()) {
		d["has_undo"] = EditorUndoRedoManager::get_singleton()->has_undo();
		d["has_redo"] = EditorUndoRedoManager::get_singleton()->has_redo();
	}
	return d;
}

Dictionary _build_project_info() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded (engine may be in Project Manager mode).", "no_project");
	}
	Dictionary d;
	d["name"] = String(GLOBAL_GET("application/config/name"));
	d["description"] = String(GLOBAL_GET("application/config/description"));
	d["path"] = ps->get_resource_path();
	d["main_scene"] = String(GLOBAL_GET("application/run/main_scene"));
	d["icon"] = String(GLOBAL_GET("application/config/icon"));
	d["renderer"] = String(GLOBAL_GET("rendering/renderer/rendering_method"));
	d["viewport_width"] = int(GLOBAL_GET("display/window/size/viewport_width"));
	d["viewport_height"] = int(GLOBAL_GET("display/window/size/viewport_height"));

	Array autoloads;
	Array input_actions;
	List<PropertyInfo> props;
	ps->get_property_list(&props);
	for (const PropertyInfo &pi : props) {
		if (pi.name.begins_with("autoload/")) {
			Dictionary a;
			a["name"] = pi.name.substr(9);
			a["value"] = String(ps->get_setting(pi.name));
			autoloads.push_back(a);
		} else if (pi.name.begins_with("input/")) {
			input_actions.push_back(pi.name.substr(6));
		}
	}
	d["autoloads"] = autoloads;
	d["input_actions"] = input_actions;
	return mcp::make_ok(d);
}

Dictionary _tool_engine_get_status(const Dictionary &p_args) {
	return mcp::make_ok(_build_engine_status());
}

Dictionary _tool_project_create(const Dictionary &p_args) {
	String path = String(p_args.get("path", "")).strip_edges().replace("\\", "/");
	String name = String(p_args.get("name", "")).strip_edges();
	String renderer = String(p_args.get("renderer", "forward_plus")).strip_edges().to_lower();
	bool create_default_dirs = bool(p_args.get("create_default_dirs", true));
	bool create_main_scene = bool(p_args.get("create_main_scene", true));
	String main_scene_root_type = String(p_args.get("main_scene_root_type", "Node3D")).strip_edges();
	bool register_in_manager = bool(p_args.get("register_in_manager", true));

	if (path.is_empty() || path.is_relative_path()) {
		return mcp::make_error("Parameter 'path' must be an absolute directory path.", "invalid_path");
	}
	if (name.is_empty()) {
		name = path.get_file();
	}
	if (renderer != "forward_plus" && renderer != "mobile" && renderer != "gl_compatibility") {
		return mcp::make_error("Parameter 'renderer' must be 'forward_plus', 'mobile', or 'gl_compatibility'.", "invalid_renderer");
	}

	Error dir_err = DirAccess::make_dir_recursive_absolute(path);
	if (dir_err != OK && dir_err != ERR_ALREADY_EXISTS) {
		return mcp::make_error(vformat("Could not create project directory '%s' (error %d).", path, (int)dir_err), "io_error");
	}

	String project_file = path.path_join("project.godot");
	if (FileAccess::exists(project_file) && !bool(p_args.get("overwrite", false))) {
		return mcp::make_error(vformat("A project already exists at '%s'. Pass overwrite=true to replace project.godot.", project_file), "already_exists");
	}

	PackedStringArray features = ProjectSettings::get_required_features();
	ProjectSettings::CustomMap initial_settings;
	initial_settings["rendering/renderer/rendering_method"] = renderer;
	if (renderer == "forward_plus") {
		features.push_back("Forward Plus");
	} else if (renderer == "mobile") {
		features.push_back("Mobile");
	} else if (renderer == "gl_compatibility") {
		features.push_back("GL Compatibility");
		initial_settings["rendering/renderer/rendering_method.mobile"] = "gl_compatibility";
	}
	features.sort();
	initial_settings["application/config/features"] = features;
	initial_settings["application/config/name"] = name;
	initial_settings["application/config/icon"] = "res://icon.svg";

	if (create_main_scene) {
		initial_settings["application/run/main_scene"] = "res://scenes/main.tscn";
	}

	Error save_err = ProjectSettings::get_singleton()->save_custom(project_file, initial_settings, Vector<String>(), false);
	if (save_err != OK) {
		return mcp::make_error(vformat("Failed to write '%s' (error %d).", project_file, (int)save_err), "io_error");
	}

	// Default project icon.
	Ref<FileAccess> icon_file = FileAccess::open(path.path_join("icon.svg"), FileAccess::WRITE);
	if (icon_file.is_valid()) {
		icon_file->store_string(get_default_project_icon());
	}

	// Default .editorconfig.
	Ref<FileAccess> ec = FileAccess::open(path.path_join(".editorconfig"), FileAccess::WRITE);
	if (ec.is_valid()) {
		ec->store_line("root = true");
		ec->store_line("");
		ec->store_line("[*]");
		ec->store_line("charset = utf-8");
	}

	Array created_dirs;
	if (create_default_dirs) {
		static const char *default_dirs[] = { "scenes", "scripts", "assets", "assets/audio", "assets/materials", "assets/models", "assets/textures" };
		for (const char *sub : default_dirs) {
			String full = path.path_join(sub);
			if (DirAccess::make_dir_recursive_absolute(full) == OK) {
				created_dirs.push_back(String("res://") + sub);
			}
		}
	}

	String main_scene_path;
	if (create_main_scene) {
		DirAccess::make_dir_recursive_absolute(path.path_join("scenes"));
		main_scene_path = path.path_join("scenes/main.tscn");
		Ref<FileAccess> sf = FileAccess::open(main_scene_path, FileAccess::WRITE);
		if (sf.is_valid()) {
			sf->store_string(vformat("[gd_scene format=3]\n\n[node name=\"Main\" type=\"%s\"]\n", main_scene_root_type));
		}
	}

	// Register project in Godot's project list so it appears in Project Manager.
	if (register_in_manager && EditorPaths::get_singleton()) {
		String projects_cfg_path = EditorPaths::get_singleton()->get_data_dir().path_join("projects.cfg");
		Ref<ConfigFile> cfg;
		cfg.instantiate();
		cfg->load(projects_cfg_path);
		cfg->set_value(path, "favorite", false);
		cfg->save(projects_cfg_path);
	}

	Dictionary out;
	out["path"] = path;
	out["project_file"] = project_file;
	out["name"] = name;
	out["renderer"] = renderer;
	out["created_dirs"] = created_dirs;
	out["main_scene"] = create_main_scene ? String("res://scenes/main.tscn") : String();
	return mcp::make_ok(out);
}

Dictionary _tool_project_list(const Dictionary &p_args) {
	Array projects;
	if (EditorPaths::get_singleton()) {
		String projects_cfg_path = EditorPaths::get_singleton()->get_data_dir().path_join("projects.cfg");
		Ref<ConfigFile> cfg;
		cfg.instantiate();
		if (cfg->load(projects_cfg_path) == OK) {
			Vector<String> sections = cfg->get_sections();
			for (int i = 0; i < sections.size(); i++) {
				String dir = sections[i];
				String godot_file = dir.path_join("project.godot");
				Dictionary item;
				item["path"] = dir;
				item["exists"] = FileAccess::exists(godot_file);
				item["favorite"] = bool(cfg->get_value(dir, "favorite", false));
				if (FileAccess::exists(godot_file)) {
					Ref<ConfigFile> pcfg;
					pcfg.instantiate();
					if (pcfg->load(godot_file) == OK) {
						item["name"] = String(pcfg->get_value("application", "config/name", dir.get_file()));
					}
				}
				projects.push_back(item);
			}
		}
	}
	Dictionary out;
	out["projects"] = projects;
	out["count"] = projects.size();
	return mcp::make_ok(out);
}

Dictionary _tool_project_open(const Dictionary &p_args) {
	String path = String(p_args.get("path", "")).strip_edges().replace("\\", "/");
	bool quit_current = bool(p_args.get("quit_current", true));
	if (path.is_empty() || !FileAccess::exists(path.path_join("project.godot"))) {
		return mcp::make_error(vformat("No project.godot found at '%s'.", path), "project_not_found");
	}

	List<String> args;
	args.push_back("--path");
	args.push_back(path);
	args.push_back("--editor");
	if (MinaMCPServer::get_singleton()) {
		args.push_back("--mcp-port");
		args.push_back(itos(MinaMCPServer::get_singleton()->get_port()));
	}

	ProcessID pid = 0;
	Error err = OS::get_singleton()->create_instance(args, &pid);
	if (err != OK) {
		return mcp::make_error(vformat("Failed to launch editor for '%s' (error %d).", path, (int)err), "launch_failed");
	}

	if (quit_current) {
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		if (st) {
			st->call_deferred("quit");
		}
	}

	Dictionary out;
	out["path"] = path;
	out["pid"] = (int64_t)pid;
	out["quit_current"] = quit_current;
	return mcp::make_ok(out);
}

Dictionary _tool_project_get_info(const Dictionary &p_args) {
	return _build_project_info();
}

Dictionary _tool_project_get_setting(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded.", "no_project");
	}
	String name = String(p_args.get("name", "")).strip_edges();
	if (name.is_empty()) {
		return mcp::make_error("Parameter 'name' is required.", "invalid_argument");
	}
	if (!ps->has_setting(name)) {
		return mcp::make_error(vformat("Project setting '%s' does not exist.", name), "not_found");
	}
	Variant val = ps->get_setting(name);
	Dictionary out;
	out["name"] = name;
	out["type"] = Variant::get_type_name(val.get_type());
	out["value"] = mcp::variant_to_json(val);
	return mcp::make_ok(out);
}

Dictionary _tool_project_set_setting(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded.", "no_project");
	}
	String name = String(p_args.get("name", "")).strip_edges();
	if (name.is_empty()) {
		return mcp::make_error("Parameter 'name' is required.", "invalid_argument");
	}
	bool save = bool(p_args.get("save", true));
	Variant raw_val = p_args.get("value", Variant());

	Variant::Type hint_type = Variant::NIL;
	if (ps->has_setting(name)) {
		hint_type = ps->get_setting(name).get_type();
	}
	String conv_err;
	Variant converted = mcp::json_to_variant(raw_val, hint_type, &conv_err);
	if (!conv_err.is_empty()) {
		return mcp::make_error(conv_err, "conversion_error");
	}

	ps->set_setting(name, converted);
	if (save) {
		Error err = ps->save();
		if (err != OK) {
			return mcp::make_error(vformat("Updated setting in memory but failed to save project.godot (error %d).", (int)err), "save_failed");
		}
	}

	Dictionary out;
	out["name"] = name;
	out["value"] = mcp::variant_to_json(ps->get_setting(name));
	out["saved"] = save;
	return mcp::make_ok(out);
}

Dictionary _tool_project_list_settings(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded.", "no_project");
	}
	String prefix = String(p_args.get("prefix", "")).strip_edges();
	int limit = CLAMP(int(p_args.get("limit", 100)), 1, 500);

	Array items;
	List<PropertyInfo> props;
	ps->get_property_list(&props);
	for (const PropertyInfo &pi : props) {
		if (!prefix.is_empty() && !pi.name.begins_with(prefix)) {
			continue;
		}
		if (items.size() >= limit) {
			break;
		}
		Variant val = ps->get_setting(pi.name);
		Dictionary item;
		item["name"] = pi.name;
		item["type"] = Variant::get_type_name(val.get_type());
		item["value"] = mcp::variant_to_json(val);
		items.push_back(item);
	}

	Dictionary out;
	out["prefix"] = prefix;
	out["settings"] = items;
	out["count"] = items.size();
	return mcp::make_ok(out);
}

Dictionary _tool_project_configure_input_action(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded.", "no_project");
	}
	String action = String(p_args.get("action", "")).strip_edges();
	String operation = String(p_args.get("operation", "set")).strip_edges().to_lower();
	if (action.is_empty()) {
		return mcp::make_error("Parameter 'action' is required.", "invalid_argument");
	}
	String setting_key = "input/" + action;

	if (operation == "remove") {
		ps->set_setting(setting_key, Variant());
		ps->save();
		Dictionary out;
		out["action"] = action;
		out["removed"] = true;
		return mcp::make_ok(out);
	}

	double deadzone = double(p_args.get("deadzone", 0.5));
	Array keys_arr = p_args.get("keys", Array());
	Array mouse_buttons = p_args.get("mouse_buttons", Array());

	Array events;
	for (int i = 0; i < keys_arr.size(); i++) {
		String key_name = String(keys_arr[i]).strip_edges();
		Key kc = find_keycode(key_name);
		if (kc == Key::NONE) {
			return mcp::make_error(vformat("Unknown key name '%s'. Examples: 'W', 'A', 'S', 'D', 'E', 'F', 'Space', 'Shift', 'Escape'.", key_name), "invalid_key");
		}
		Ref<InputEventKey> ev;
		ev.instantiate();
		ev->set_physical_keycode(kc);
		events.push_back(ev);
	}

	for (int i = 0; i < mouse_buttons.size(); i++) {
		String btn = String(mouse_buttons[i]).strip_edges().to_lower();
		MouseButton mb = MouseButton::NONE;
		if (btn == "left" || btn == "1") {
			mb = MouseButton::LEFT;
		} else if (btn == "right" || btn == "2") {
			mb = MouseButton::RIGHT;
		} else if (btn == "middle" || btn == "3") {
			mb = MouseButton::MIDDLE;
		} else if (btn == "wheel_up") {
			mb = MouseButton::WHEEL_UP;
		} else if (btn == "wheel_down") {
			mb = MouseButton::WHEEL_DOWN;
		} else {
			return mcp::make_error(vformat("Unknown mouse button '%s'. Use left, right, middle, wheel_up, wheel_down.", btn), "invalid_mouse_button");
		}
		Ref<InputEventMouseButton> mev;
		mev.instantiate();
		mev->set_button_index(mb);
		events.push_back(mev);
	}

	Dictionary action_dict;
	action_dict["deadzone"] = deadzone;
	action_dict["events"] = events;
	ps->set_setting(setting_key, action_dict);
	Error err = ps->save();
	if (err != OK) {
		return mcp::make_error(vformat("Failed to save project.godot (error %d).", (int)err), "save_failed");
	}

	Dictionary out;
	out["action"] = action;
	out["deadzone"] = deadzone;
	out["events_count"] = events.size();
	return mcp::make_ok(out);
}

Dictionary _tool_project_manage_autoload(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps || ps->get_resource_path().is_empty()) {
		return mcp::make_error("No project is currently loaded.", "no_project");
	}
	String op = String(p_args.get("operation", "list")).strip_edges().to_lower();
	if (op == "list") {
		Array autoloads;
		List<PropertyInfo> props;
		ps->get_property_list(&props);
		for (const PropertyInfo &pi : props) {
			if (pi.name.begins_with("autoload/")) {
				String val = String(ps->get_setting(pi.name));
				Dictionary a;
				a["name"] = pi.name.substr(9);
				a["singleton"] = val.begins_with("*");
				a["path"] = val.begins_with("*") ? val.substr(1) : val;
				autoloads.push_back(a);
			}
		}
		Dictionary out;
		out["autoloads"] = autoloads;
		return mcp::make_ok(out);
	}

	String name = String(p_args.get("name", "")).strip_edges();
	if (name.is_empty()) {
		return mcp::make_error("Parameter 'name' is required.", "invalid_argument");
	}
	String key = "autoload/" + name;

	if (op == "remove") {
		ps->set_setting(key, Variant());
		ps->save();
		Dictionary out;
		out["name"] = name;
		out["removed"] = true;
		return mcp::make_ok(out);
	}

	if (op == "set" || op == "add") {
		String path = mcp::normalize_res_path(String(p_args.get("path", "")));
		if (!mcp::is_safe_res_path(path)) {
			return mcp::make_error("Parameter 'path' must be a valid res:// script or scene path.", "invalid_path");
		}
		bool singleton = bool(p_args.get("singleton", true));
		String val = singleton ? ("*" + path) : path;
		ps->set_setting(key, val);
		ps->save();
		Dictionary out;
		out["name"] = name;
		out["path"] = path;
		out["singleton"] = singleton;
		return mcp::make_ok(out);
	}

	return mcp::make_error("Parameter 'operation' must be 'list', 'set', or 'remove'.", "invalid_argument");
}

} // namespace

void mcp_register_project_tools(MinaMCPServer *p_server) {
	using namespace mcp;

	{
		MinaMCPServer::ToolDefinition t;
		t.name = "engine_get_status";
		t.title = "Get Engine Status";
		t.description = "Get Mina Engine runtime status, version, mode (editor or project_manager), active project, open scenes, playback state, and MCP server info.";
		t.category = "engine";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_engine_get_status;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Absolute filesystem path where the new project directory will be created.");
		props["name"] = schema_string("Project display name.");
		Array renderers;
		renderers.push_back("forward_plus");
		renderers.push_back("mobile");
		renderers.push_back("gl_compatibility");
		props["renderer"] = schema_string("Rendering method (default: forward_plus).", renderers);
		props["create_default_dirs"] = schema_bool("Create standard scenes/, scripts/, assets/ folders (default: true).");
		props["create_main_scene"] = schema_bool("Create initial res://scenes/main.tscn and set as main scene (default: true).");
		props["main_scene_root_type"] = schema_string("Root node class for the initial main scene (default: Node3D).");
		props["register_in_manager"] = schema_bool("Register project in Project Manager list (default: true).");
		props["overwrite"] = schema_bool("Allow overwriting an existing project.godot (default: false).");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_create";
		t.title = "Create Project";
		t.description = "Create a new Mina Engine project on disk (project.godot, icon, standard directories, optional main scene). Works in both Project Manager and Editor modes.";
		t.category = "project";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_project_create;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "project_list";
		t.title = "List Known Projects";
		t.description = "List Mina Engine projects registered in the Project Manager.";
		t.category = "project";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_project_list;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Absolute directory path containing project.godot.");
		props["quit_current"] = schema_bool("Quit the current Project Manager or Editor instance after launching (default: true).");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_open";
		t.title = "Open Project in Editor";
		t.description = "Launch the Mina Engine Editor for a project directory on disk, preserving the MCP server port.";
		t.category = "project";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_project_open;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "project_get_info";
		t.title = "Get Project Info";
		t.description = "Get summary information about the currently open project: name, root path, main scene, renderer, window size, autoloads, and input actions.";
		t.category = "project";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_project_get_info;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["name"] = schema_string("ProjectSettings key, e.g. 'application/run/main_scene' or 'display/window/size/viewport_width'.");
		Array req;
		req.push_back("name");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_get_setting";
		t.title = "Get Project Setting";
		t.description = "Read a specific setting from ProjectSettings.";
		t.category = "project";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_project_get_setting;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["name"] = schema_string("ProjectSettings key to update.");
		props["value"] = schema_any("New value for the setting (primitives, arrays, objects, Vector2/3, Color, etc.).");
		props["save"] = schema_bool("Persist changes to project.godot immediately (default: true).");
		Array req;
		req.push_back("name");
		req.push_back("value");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_set_setting";
		t.title = "Set Project Setting";
		t.description = "Update a ProjectSettings key and save project.godot.";
		t.category = "project";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_project_set_setting;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["prefix"] = schema_string("Optional key prefix filter, e.g. 'application/', 'rendering/', 'physics/', 'display/'.");
		props["limit"] = schema_int("Maximum number of settings to return (default: 100, max: 500).");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_list_settings";
		t.title = "List Project Settings";
		t.description = "List ProjectSettings keys and values matching an optional prefix.";
		t.category = "project";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_project_list_settings;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["action"] = schema_string("Input action name, e.g. 'move_forward', 'interact', 'flashlight'.");
		Array ops;
		ops.push_back("set");
		ops.push_back("remove");
		props["operation"] = schema_string("'set' to create/replace action or 'remove' to delete it (default: 'set').", ops);
		props["keys"] = schema_array("Key names to bind, e.g. ['W', 'Up', 'Space', 'E', 'Shift'].", schema_string("Key name"));
		props["mouse_buttons"] = schema_array("Mouse buttons to bind: 'left', 'right', 'middle', 'wheel_up', 'wheel_down'.", schema_string("Mouse button"));
		props["deadzone"] = schema_number("Input action deadzone (default: 0.5).");
		Array req;
		req.push_back("action");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_configure_input_action";
		t.title = "Configure Input Action";
		t.description = "Create, update, or remove an InputMap action with keyboard and mouse bindings in project.godot.";
		t.category = "project";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_project_configure_input_action;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		Array ops;
		ops.push_back("list");
		ops.push_back("set");
		ops.push_back("remove");
		props["operation"] = schema_string("'list', 'set', or 'remove' (default: 'list').", ops);
		props["name"] = schema_string("Autoload singleton name (required for 'set' and 'remove').");
		props["path"] = schema_string("res:// script or scene path (required for 'set').");
		props["singleton"] = schema_bool("Enable as global singleton variable (default: true).");

		MinaMCPServer::ToolDefinition t;
		t.name = "project_manage_autoload";
		t.title = "Manage Autoloads";
		t.description = "List, add/update, or remove project Autoload singletons in project.godot.";
		t.category = "project";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_project_manage_autoload;
		p_server->register_tool(t);
	}

	// Register core live MCP resources so clients can subscribe/read engine & project state directly.
	{
		MinaMCPServer::ResourceDefinition r;
		r.uri = "mina://engine/status";
		r.name = "Mina Engine Status";
		r.description = "Live status of the running Mina Engine instance, MCP server, open scenes, and playback state.";
		r.mime_type = "application/json";
		r.reader = _build_engine_status;
		p_server->register_resource(r);
	}
	{
		MinaMCPServer::ResourceDefinition r;
		r.uri = "mina://project/info";
		r.name = "Current Project Info";
		r.description = "Metadata, settings summary, autoloads, and input actions of the currently loaded project.";
		r.mime_type = "application/json";
		r.reader = _build_project_info;
		p_server->register_resource(r);
	}
	{
		MinaMCPServer::ResourceDefinition r;
		r.uri = "mina://logs/recent";
		r.name = "Recent Engine Logs";
		r.description = "Most recent 100 captured engine stdout messages, warnings, and errors.";
		r.mime_type = "application/json";
		r.reader = []() -> Dictionary {
			Dictionary d;
			if (MinaMCPServer::get_singleton()) {
				d["logs"] = MinaMCPServer::get_singleton()->get_logs("all", 0, 100);
			}
			return d;
		};
		p_server->register_resource(r);
	}
}

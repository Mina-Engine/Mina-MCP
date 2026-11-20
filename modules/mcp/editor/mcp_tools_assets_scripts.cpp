/**************************************************************************/
/*  mcp_tools_assets_scripts.cpp                                          */
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
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/node.h"

namespace {

bool _require_project(Dictionary &r_err) {
	if (!ProjectSettings::get_singleton() || ProjectSettings::get_singleton()->get_resource_path().is_empty()) {
		r_err = mcp::make_error("No project is currently loaded.", "no_project");
		return false;
	}
	return true;
}

void _collect_files_recursive(const String &p_dir, bool p_recursive, const Vector<String> &p_exts, const String &p_search_lower, int p_limit, Array &r_files) {
	if (r_files.size() >= p_limit) {
		return;
	}
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	String name = da->get_next();
	while (!name.is_empty() && r_files.size() < p_limit) {
		if (name == "." || name == ".." || name == ".godot" || name == ".git") {
			name = da->get_next();
			continue;
		}
		String full = p_dir.ends_with("/") ? (p_dir + name) : (p_dir + "/" + name);
		if (da->current_is_dir()) {
			if (p_recursive) {
				_collect_files_recursive(full, true, p_exts, p_search_lower, p_limit, r_files);
			}
		} else {
			if (name.ends_with(".import") || name.ends_with(".uid")) {
				name = da->get_next();
				continue;
			}
			String ext = name.get_extension().to_lower();
			bool ext_ok = p_exts.is_empty() || p_exts.has(ext);
			bool search_ok = p_search_lower.is_empty() || full.to_lower().contains(p_search_lower);
			if (ext_ok && search_ok) {
				Dictionary entry;
				entry["path"] = full;
				entry["name"] = name;
				entry["extension"] = ext;
				if (EditorFileSystem::get_singleton()) {
					entry["type"] = String(EditorFileSystem::get_singleton()->get_file_type(full));
				}
				r_files.push_back(entry);
			}
		}
		name = da->get_next();
	}
	da->list_dir_end();
}

Dictionary _tool_asset_list_files(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String dir = String(p_args.get("directory", "res://")).strip_edges();
	if (dir.is_empty()) {
		dir = "res://";
	} else {
		dir = mcp::normalize_res_path(dir);
	}
	if (!mcp::is_safe_res_path(dir, true)) {
		return mcp::make_error("Parameter 'directory' must be a valid res:// path.", "invalid_path");
	}
	bool recursive = bool(p_args.get("recursive", true));
	String search = String(p_args.get("search", "")).strip_edges().to_lower();
	int limit = CLAMP(int(p_args.get("limit", 250)), 1, 2000);

	Vector<String> exts;
	Array ext_arr = p_args.get("extensions", Array());
	for (int i = 0; i < ext_arr.size(); i++) {
		String e = String(ext_arr[i]).strip_edges().to_lower().trim_prefix(".");
		if (!e.is_empty()) {
			exts.push_back(e);
		}
	}

	Array files;
	_collect_files_recursive(dir, recursive, exts, search, limit, files);

	Dictionary out;
	out["directory"] = dir;
	out["count"] = files.size();
	out["truncated"] = files.size() >= limit;
	out["files"] = files;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_get_info(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// path.", "invalid_path");
	}
	if (!FileAccess::exists(path)) {
		return mcp::make_error(vformat("File does not exist: '%s'.", path), "not_found");
	}

	Dictionary out;
	out["path"] = path;
	out["absolute_path"] = ProjectSettings::get_singleton()->globalize_path(path);
	out["extension"] = path.get_extension().to_lower();
	out["modified_time"] = (int64_t)FileAccess::get_modified_time(path);
	Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ);
	if (fa.is_valid()) {
		out["size_bytes"] = (int64_t)fa->get_length();
	}
	if (EditorFileSystem::get_singleton()) {
		out["type"] = String(EditorFileSystem::get_singleton()->get_file_type(path));
		int idx = 0;
		EditorFileSystemDirectory *fsd = EditorFileSystem::get_singleton()->find_file(path, &idx);
		if (fsd) {
			out["import_valid"] = fsd->get_file_import_is_valid(idx);
			out["uid"] = ResourceUID::get_singleton()->id_to_text(fsd->get_file_uid(idx));
			Vector<String> deps = fsd->get_file_deps(idx);
			Array dep_arr;
			for (int i = 0; i < deps.size(); i++) {
				dep_arr.push_back(deps[i]);
			}
			out["dependencies"] = dep_arr;
		}
	}
	return mcp::make_ok(out);
}

Dictionary _tool_asset_create_resource(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String cls = String(p_args.get("type", "")).strip_edges();
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	Dictionary props = p_args.get("properties", Dictionary());

	if (cls.is_empty() || !ClassDB::class_exists(cls) || !ClassDB::is_parent_class(cls, "Resource") || !ClassDB::can_instantiate(cls)) {
		return mcp::make_error(vformat("'%s' is not an instantiable Resource class.", cls), "invalid_class");
	}
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// path (e.g. 'res://assets/materials/wall.tres').", "invalid_path");
	}

	Object *obj = ClassDB::instantiate(cls);
	Ref<Resource> res = Object::cast_to<Resource>(obj);
	if (res.is_null()) {
		if (obj) {
			memdelete(obj);
		}
		return mcp::make_error(vformat("Failed to instantiate Resource '%s'.", cls), "instantiate_failed");
	}

	Array applied, warnings;
	Array keys = props.keys();
	for (int i = 0; i < keys.size(); i++) {
		String k = String(keys[i]);
		PropertyInfo pinfo;
		Variant::Type hint = Variant::NIL;
		if (ClassDB::get_property_info(cls, k, &pinfo)) {
			hint = pinfo.type;
		}
		String conv_err;
		Variant v = mcp::json_to_variant(props[k], hint, &conv_err);
		if (!conv_err.is_empty()) {
			warnings.push_back(vformat("%s: %s", k, conv_err));
			continue;
		}
		res->set(k, v);
		applied.push_back(k);
	}

	String abs_dir = ProjectSettings::get_singleton()->globalize_path(path.get_base_dir());
	DirAccess::make_dir_recursive_absolute(abs_dir);
	Error save_err = ResourceSaver::save(res, path);
	if (save_err != OK) {
		return mcp::make_error(vformat("Failed to save resource to '%s' (error %d).", path, (int)save_err), "save_failed");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}

	Dictionary out;
	out["path"] = path;
	out["type"] = cls;
	out["applied_properties"] = applied;
	if (!warnings.is_empty()) {
		out["warnings"] = warnings;
	}
	return mcp::make_ok(out);
}

Dictionary _tool_asset_inspect_resource(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path) || !ResourceLoader::exists(path)) {
		return mcp::make_error(vformat("Resource does not exist: '%s'.", path), "not_found");
	}
	Ref<Resource> res = ResourceLoader::load(path);
	if (res.is_null()) {
		return mcp::make_error(vformat("Failed to load resource '%s'.", path), "load_failed");
	}

	String filter = String(p_args.get("filter", "")).strip_edges().to_lower();
	Dictionary props_out;
	List<PropertyInfo> plist;
	res->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_SUBGROUP)) {
			continue;
		}
		if (!(pi.usage & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE))) {
			continue;
		}
		if (!filter.is_empty() && !pi.name.to_lower().contains(filter)) {
			continue;
		}
		Dictionary entry;
		entry["type"] = Variant::get_type_name(pi.type);
		entry["value"] = mcp::variant_to_json(res->get(pi.name));
		props_out[pi.name] = entry;
	}

	Dictionary out;
	out["path"] = path;
	out["class"] = res->get_class();
	out["properties"] = props_out;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_update_resource(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	Dictionary props = p_args.get("properties", Dictionary());
	if (!mcp::is_safe_res_path(path) || !ResourceLoader::exists(path)) {
		return mcp::make_error(vformat("Resource does not exist: '%s'.", path), "not_found");
	}
	Ref<Resource> res = ResourceLoader::load(path);
	if (res.is_null()) {
		return mcp::make_error(vformat("Failed to load resource '%s'.", path), "load_failed");
	}

	Array applied, warnings;
	Array keys = props.keys();
	for (int i = 0; i < keys.size(); i++) {
		String k = String(keys[i]);
		PropertyInfo pinfo;
		Variant::Type hint = Variant::NIL;
		if (ClassDB::get_property_info(res->get_class_name(), k, &pinfo)) {
			hint = pinfo.type;
		}
		String conv_err;
		Variant v = mcp::json_to_variant(props[k], hint, &conv_err);
		if (!conv_err.is_empty()) {
			warnings.push_back(vformat("%s: %s", k, conv_err));
			continue;
		}
		res->set(k, v);
		applied.push_back(k);
	}

	Error save_err = ResourceSaver::save(res, path);
	if (save_err != OK) {
		return mcp::make_error(vformat("Failed to save resource '%s' (error %d).", path, (int)save_err), "save_failed");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}
	Dictionary out;
	out["path"] = path;
	out["applied"] = applied;
	if (!warnings.is_empty()) {
		out["warnings"] = warnings;
	}
	return mcp::make_ok(out);
}

Dictionary _tool_asset_create_directory(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// directory path.", "invalid_path");
	}
	String abs = ProjectSettings::get_singleton()->globalize_path(path);
	Error derr = DirAccess::make_dir_recursive_absolute(abs);
	if (derr != OK && derr != ERR_ALREADY_EXISTS) {
		return mcp::make_error(vformat("Failed to create directory '%s' (error %d).", path, (int)derr), "io_error");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	Dictionary out;
	out["path"] = path;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_move_file(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String from = mcp::normalize_res_path(String(p_args.get("from", "")));
	String to = mcp::normalize_res_path(String(p_args.get("to", "")));
	if (!mcp::is_safe_res_path(from) || !mcp::is_safe_res_path(to)) {
		return mcp::make_error("Parameters 'from' and 'to' must be valid res:// paths.", "invalid_path");
	}
	String abs_from = ProjectSettings::get_singleton()->globalize_path(from);
	String abs_to = ProjectSettings::get_singleton()->globalize_path(to);
	DirAccess::make_dir_recursive_absolute(abs_to.get_base_dir());
	Error rerr = DirAccess::rename_absolute(abs_from, abs_to);
	if (rerr != OK) {
		return mcp::make_error(vformat("Failed to move '%s' to '%s' (error %d).", from, to, (int)rerr), "io_error");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	Dictionary out;
	out["from"] = from;
	out["to"] = to;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_copy_file(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String from = mcp::normalize_res_path(String(p_args.get("from", "")));
	String to = mcp::normalize_res_path(String(p_args.get("to", "")));
	if (!mcp::is_safe_res_path(from) || !mcp::is_safe_res_path(to)) {
		return mcp::make_error("Parameters 'from' and 'to' must be valid res:// paths.", "invalid_path");
	}
	String abs_from = ProjectSettings::get_singleton()->globalize_path(from);
	String abs_to = ProjectSettings::get_singleton()->globalize_path(to);
	DirAccess::make_dir_recursive_absolute(abs_to.get_base_dir());
	Error cerr = DirAccess::copy_absolute(abs_from, abs_to);
	if (cerr != OK) {
		return mcp::make_error(vformat("Failed to copy '%s' to '%s' (error %d).", from, to, (int)cerr), "io_error");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	Dictionary out;
	out["from"] = from;
	out["to"] = to;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_delete_file(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// path.", "invalid_path");
	}
	if (path == "res://project.godot") {
		return mcp::make_error("Refusing to delete project.godot.", "protected_file");
	}
	String abs = ProjectSettings::get_singleton()->globalize_path(path);
	Error rerr = DirAccess::remove_absolute(abs);
	if (rerr != OK) {
		return mcp::make_error(vformat("Failed to delete '%s' (error %d).", path, (int)rerr), "io_error");
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	Dictionary out;
	out["deleted"] = path;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_scan_filesystem(const Dictionary &p_args) {
	if (!EditorFileSystem::get_singleton()) {
		return mcp::make_error("EditorFileSystem is not available.", "editor_required");
	}
	bool full = bool(p_args.get("full_scan", false));
	if (full) {
		EditorFileSystem::get_singleton()->scan();
	} else {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	Dictionary out;
	out["full_scan"] = full;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_reimport(const Dictionary &p_args) {
	if (!EditorFileSystem::get_singleton()) {
		return mcp::make_error("EditorFileSystem is not available.", "editor_required");
	}
	Array raw_paths = p_args.get("paths", Array());
	Vector<String> paths;
	for (int i = 0; i < raw_paths.size(); i++) {
		String p = mcp::normalize_res_path(String(raw_paths[i]));
		if (mcp::is_safe_res_path(p)) {
			paths.push_back(p);
		}
	}
	if (paths.is_empty()) {
		return mcp::make_error("Parameter 'paths' must contain at least one valid res:// path.", "invalid_argument");
	}
	EditorFileSystem::get_singleton()->reimport_files(paths);
	Dictionary out;
	out["reimported_count"] = paths.size();
	return mcp::make_ok(out);
}

Dictionary _tool_script_read(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// path.", "invalid_path");
	}
	if (!FileAccess::exists(path)) {
		return mcp::make_error(vformat("File does not exist: '%s'.", path), "not_found");
	}
	Error ferr = OK;
	String text = FileAccess::get_file_as_string(path, &ferr);
	if (ferr != OK) {
		return mcp::make_error(vformat("Failed to read '%s' (error %d).", path, (int)ferr), "io_error");
	}

	PackedStringArray lines = text.split("\n");
	int total_lines = lines.size();
	int start_line = MAX(1, int(p_args.get("start_line", 1)));
	int end_line = p_args.has("end_line") ? MIN(total_lines, int(p_args["end_line"])) : total_lines;

	String content;
	if (start_line == 1 && end_line == total_lines) {
		content = text;
	} else {
		for (int i = start_line - 1; i < end_line && i < total_lines; i++) {
			if (i > start_line - 1) {
				content += "\n";
			}
			content += lines[i];
		}
	}

	Dictionary out;
	out["path"] = path;
	out["total_lines"] = total_lines;
	out["start_line"] = start_line;
	out["end_line"] = end_line;
	out["content"] = content;
	return mcp::make_ok(out);
}

Dictionary _tool_script_write(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	String content = String(p_args.get("content", ""));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'path' must be a valid res:// path.", "invalid_path");
	}

	String abs_path = ProjectSettings::get_singleton()->globalize_path(path);
	DirAccess::make_dir_recursive_absolute(abs_path.get_base_dir());
	Error ferr = OK;
	Ref<FileAccess> fa = FileAccess::open(path, FileAccess::WRITE, &ferr);
	if (fa.is_null() || ferr != OK) {
		return mcp::make_error(vformat("Failed to write '%s' (error %d).", path, (int)ferr), "io_error");
	}
	fa->store_string(content);
	fa->close();

	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->update_file(path);
	}
	// Reload cached Script resource in-memory if already loaded.
	if (path.ends_with(".gd") || path.ends_with(".gdshader")) {
		Ref<Resource> res = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REPLACE);
		if (res.is_valid()) {
			Ref<Script> scr = res;
			if (scr.is_valid()) {
				scr->reload(true);
			}
		}
	}

	Dictionary out;
	out["path"] = path;
	out["bytes_written"] = content.utf8().length();
	out["lines"] = content.count("\n") + 1;
	return mcp::make_ok(out);
}

Dictionary _tool_script_patch(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_project(err)) {
		return err;
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	String find_text = String(p_args.get("find", ""));
	String replace_text = String(p_args.get("replace", ""));
	bool replace_all = bool(p_args.get("replace_all", false));

	if (!mcp::is_safe_res_path(path) || !FileAccess::exists(path)) {
		return mcp::make_error(vformat("File does not exist: '%s'.", path), "not_found");
	}
	if (find_text.is_empty()) {
		return mcp::make_error("Parameter 'find' must be non-empty.", "invalid_argument");
	}

	Error ferr = OK;
	String content = FileAccess::get_file_as_string(path, &ferr);
	if (ferr != OK) {
		return mcp::make_error(vformat("Failed to read '%s'.", path), "io_error");
	}

	int occurrences = content.count(find_text);
	if (occurrences == 0) {
		return mcp::make_error("Target text was not found in the file.", "target_not_found");
	}

	String updated;
	if (replace_all) {
		updated = content.replace(find_text, replace_text);
	} else {
		int pos = content.find(find_text);
		updated = content.substr(0, pos) + replace_text + content.substr(pos + find_text.length());
	}

	Dictionary write_args;
	write_args["path"] = path;
	write_args["content"] = updated;
	Dictionary res = _tool_script_write(write_args);
	res["replacements"] = replace_all ? occurrences : 1;
	return res;
}

Dictionary _tool_script_attach(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) {
		return mcp::make_error("EditorInterface is not available.", "editor_required");
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return mcp::make_error("No scene is currently open.", "no_scene");
	}
	String node_path = String(p_args.get("node_path", ".")).strip_edges();
	String script_path = mcp::normalize_res_path(String(p_args.get("script_path", "")));
	if (!mcp::is_safe_res_path(script_path)) {
		return mcp::make_error("Parameter 'script_path' must be a valid res:// script path.", "invalid_path");
	}

	Node *target = (node_path.is_empty() || node_path == "." || node_path == "/") ? root : root->get_node_or_null(NodePath(node_path));
	if (!target) {
		return mcp::make_error(vformat("Node not found at '%s'.", node_path), "node_not_found");
	}

	// Optionally create the script file if 'content' was supplied or create_if_missing is true.
	if (p_args.has("content")) {
		Dictionary w;
		w["path"] = script_path;
		w["content"] = String(p_args["content"]);
		Dictionary wres = _tool_script_write(w);
		if (!bool(wres.get("ok", false))) {
			return wres;
		}
	} else if (!FileAccess::exists(script_path) && bool(p_args.get("create_if_missing", false))) {
		Dictionary w;
		w["path"] = script_path;
		w["content"] = vformat("extends %s\n\nfunc _ready() -> void:\n\tpass\n", target->get_class());
		Dictionary wres = _tool_script_write(w);
		if (!bool(wres.get("ok", false))) {
			return wres;
		}
	}

	Ref<Script> scr = ResourceLoader::load(script_path, "Script", ResourceFormatLoader::CACHE_MODE_REPLACE);
	if (scr.is_null()) {
		return mcp::make_error(vformat("Could not load script '%s'.", script_path), "load_failed");
	}
	target->set_script(scr);
	EditorInterface::get_singleton()->mark_scene_as_unsaved();

	Dictionary out;
	out["node_path"] = node_path;
	out["script_path"] = script_path;
	out["valid"] = scr->is_valid();
	return mcp::make_ok(out);
}

Dictionary _tool_script_validate(const Dictionary &p_args) {
	String path = String(p_args.get("path", "")).strip_edges();
	String code = String(p_args.get("content", ""));

	if (code.is_empty()) {
		if (path.is_empty()) {
			return mcp::make_error("Provide either 'path' or 'content'.", "invalid_argument");
		}
		path = mcp::normalize_res_path(path);
		if (!mcp::is_safe_res_path(path) || !FileAccess::exists(path)) {
			return mcp::make_error(vformat("Script file does not exist: '%s'.", path), "not_found");
		}
		Error ferr = OK;
		code = FileAccess::get_file_as_string(path, &ferr);
		if (ferr != OK) {
			return mcp::make_error(vformat("Failed to read '%s'.", path), "io_error");
		}
	}

	ScriptLanguage *gdscript_lang = nullptr;
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		ScriptLanguage *lang = ScriptServer::get_language(i);
		if (lang && lang->get_extension() == "gd") {
			gdscript_lang = lang;
			break;
		}
	}
	if (!gdscript_lang) {
		return mcp::make_error("GDScript language backend is not available.", "unavailable");
	}

	List<String> functions;
	List<ScriptLanguage::ScriptError> errors;
	List<ScriptLanguage::Warning> warnings;
	HashSet<int> safe_lines;
	bool valid = gdscript_lang->validate(code, path, &functions, &errors, &warnings, &safe_lines);

	Array err_arr;
	for (const ScriptLanguage::ScriptError &e : errors) {
		Dictionary d;
		d["line"] = e.line;
		d["column"] = e.column;
		d["message"] = e.message;
		err_arr.push_back(d);
	}
	Array warn_arr;
	for (const ScriptLanguage::Warning &w : warnings) {
		Dictionary d;
		d["start_line"] = w.start_line;
		d["end_line"] = w.end_line;
		d["code"] = w.string_code;
		d["message"] = w.message;
		warn_arr.push_back(d);
	}
	Array fn_arr;
	for (const String &fn : functions) {
		fn_arr.push_back(fn);
	}

	Dictionary out;
	out["path"] = path;
	out["valid"] = valid;
	out["errors"] = err_arr;
	out["warnings"] = warn_arr;
	out["functions"] = fn_arr;
	return mcp::make_ok(out);
}

Dictionary _tool_script_open_in_editor(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) {
		return mcp::make_error("EditorInterface is not available.", "editor_required");
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	int line = int(p_args.get("line", 1));
	if (!mcp::is_safe_res_path(path) || !ResourceLoader::exists(path)) {
		return mcp::make_error(vformat("Script does not exist: '%s'.", path), "not_found");
	}
	Ref<Script> scr = ResourceLoader::load(path);
	if (scr.is_null()) {
		return mcp::make_error(vformat("Could not load script '%s'.", path), "load_failed");
	}
	EditorInterface::get_singleton()->edit_script(scr, line, 0, true);
	Dictionary out;
	out["path"] = path;
	out["line"] = line;
	return mcp::make_ok(out);
}

} // namespace

void mcp_register_assets_scripts_tools(MinaMCPServer *p_server) {
	using namespace mcp;

	{
		Dictionary props;
		props["directory"] = schema_string("Directory under res:// to list (default: 'res://').");
		props["recursive"] = schema_bool("Recurse into subdirectories (default: true).");
		props["extensions"] = schema_array("Optional list of file extensions to include (e.g. ['tscn', 'gd', 'wav', 'ogg', 'glb']).", schema_string("Extension"));
		props["search"] = schema_string("Optional case-insensitive path substring filter.");
		props["limit"] = schema_int("Maximum results to return (default: 250, max: 2000).");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_list_files";
		t.title = "List Project Files";
		t.description = "List and search project assets, scenes, scripts, audio, meshes, and resources under res://.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_asset_list_files;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Project file path under res://.");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_get_info";
		t.title = "Get Asset Info";
		t.description = "Get metadata for a project file: resource type, UID, dependencies, import status, size, and modified time.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_get_info;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["type"] = schema_string("Resource class name (e.g. 'StandardMaterial3D', 'BoxMesh', 'Environment', 'ShaderMaterial', 'Theme', 'StyleBoxFlat').");
		props["path"] = schema_string("Destination res:// path (e.g. 'res://assets/materials/dark_wood.tres').");
		props["properties"] = schema_object("Initial properties to set on the resource before saving.");
		Array req;
		req.push_back("type");
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_create_resource";
		t.title = "Create Resource Asset";
		t.description = "Create and save any Godot/Mina Resource (.tres) with initial properties via ResourceSaver.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_create_resource;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Resource path under res://.");
		props["filter"] = schema_string("Optional property name substring filter.");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_inspect_resource";
		t.title = "Inspect Resource";
		t.description = "Load any resource under res:// and inspect its class and properties.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_inspect_resource;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Resource path under res://.");
		props["properties"] = schema_object("Properties to update on the resource.");
		Array req;
		req.push_back("path");
		req.push_back("properties");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_update_resource";
		t.title = "Update Resource";
		t.description = "Modify properties of an existing resource (.tres) and save it back to disk.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_update_resource;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Directory path under res:// to create.");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_create_directory";
		t.title = "Create Directory";
		t.description = "Create a directory (recursively) inside the project under res://.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_create_directory;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["from"] = schema_string("Source res:// path.");
		props["to"] = schema_string("Destination res:// path.");
		Array req;
		req.push_back("from");
		req.push_back("to");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_move_file";
		t.title = "Move/Rename Asset";
		t.description = "Move or rename a file or folder inside res:// and refresh EditorFileSystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_move_file;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["from"] = schema_string("Source res:// file path.");
		props["to"] = schema_string("Destination res:// file path.");
		Array req;
		req.push_back("from");
		req.push_back("to");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_copy_file";
		t.title = "Copy Asset";
		t.description = "Copy a file inside res:// and refresh EditorFileSystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_copy_file;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("File or empty directory res:// path to delete.");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_delete_file";
		t.title = "Delete Asset";
		t.description = "Delete a file under res:// (project.godot is protected) and refresh EditorFileSystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_delete_file;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["full_scan"] = schema_bool("Perform a full filesystem rescan instead of incremental scan_changes (default: false).");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_scan_filesystem";
		t.title = "Scan Project Filesystem";
		t.description = "Trigger EditorFileSystem scan so newly added or modified external assets are imported.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_asset_scan_filesystem;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["paths"] = schema_array("List of res:// asset paths to reimport.", schema_string("res:// path"));
		Array req;
		req.push_back("paths");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_reimport";
		t.title = "Reimport Assets";
		t.description = "Force reimport of specified project assets (textures, 3D models, audio, etc.) via EditorFileSystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_reimport;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Project text/script/shader path under res://.");
		props["start_line"] = schema_int("1-based starting line number (optional).");
		props["end_line"] = schema_int("1-based ending line number inclusive (optional).");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_read";
		t.title = "Read Script / Text File";
		t.description = "Read a GDScript (.gd), shader (.gdshader), scene (.tscn), resource (.tres), or text file under res://.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_script_read;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Destination res:// path (e.g. 'res://scripts/player.gd').");
		props["content"] = schema_string("Full UTF-8 text content to write.");
		Array req;
		req.push_back("path");
		req.push_back("content");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_write";
		t.title = "Write Script / Text File";
		t.description = "Create or overwrite a script, shader, or text file under res://, reloading it in the engine and updating EditorFileSystem.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_script_write;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("Target res:// script or text file path.");
		props["find"] = schema_string("Exact text block to find.");
		props["replace"] = schema_string("Replacement text.");
		props["replace_all"] = schema_bool("Replace all occurrences instead of first match only (default: false).");
		Array req;
		req.push_back("path");
		req.push_back("find");
		req.push_back("replace");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_patch";
		t.title = "Patch Script";
		t.description = "Perform targeted search-and-replace editing on a script or text file under res://.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_script_patch;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["node_path"] = schema_string("Node path in the edited scene (default: '.').");
		props["script_path"] = schema_string("res:// path to the script to attach.");
		props["content"] = schema_string("Optional script source code to write to script_path before attaching.");
		props["create_if_missing"] = schema_bool("Create a minimal stub script if script_path does not exist (default: false).");
		Array req;
		req.push_back("node_path");
		req.push_back("script_path");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_attach";
		t.title = "Attach Script to Node";
		t.description = "Attach a script to a node in the edited scene, optionally creating or writing the script in the same call.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_script_attach;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("res:// path to a .gd script file to validate.");
		props["content"] = schema_string("Optional raw GDScript source code to validate directly without saving.");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_validate";
		t.title = "Validate GDScript";
		t.description = "Parse and validate GDScript using the engine's internal GDScript compiler, returning exact syntax errors, warnings, and functions.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_script_validate;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["path"] = schema_string("res:// script path.");
		props["line"] = schema_int("Line number to jump to (default: 1).");
		Array req;
		req.push_back("path");

		MinaMCPServer::ToolDefinition t;
		t.name = "script_open_in_editor";
		t.title = "Open Script in Editor";
		t.description = "Open a script at a specific line in Mina Engine's built-in Script Editor.";
		t.category = "scripts";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_script_open_in_editor;
		p_server->register_tool(t);
	}
}

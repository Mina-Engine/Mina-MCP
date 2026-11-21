/**************************************************************************/
/*  mcp_editor_plugin.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "scene/main/node.h"

class Button;
class CheckBox;
class EditorDock;
class HFlowContainer;
class ItemList;
class Label;
class LineEdit;
class MCPRuntimeDebuggerPlugin;
class RichTextLabel;
class SpinBox;

class MCPEditorPlugin : public EditorPlugin {
	GDCLASS(MCPEditorPlugin, EditorPlugin);

	Ref<MCPRuntimeDebuggerPlugin> runtime_debugger_plugin;
	EditorDock *dock = nullptr;
	Label *status_label = nullptr;
	Label *error_label = nullptr;
	LineEdit *endpoint_field = nullptr;
	SpinBox *port_spin = nullptr;
	CheckBox *auto_start_check = nullptr;
	Button *toggle_button = nullptr;
	Button *restart_button = nullptr;
	Label *stats_label = nullptr;
	HFlowContainer *categories_flow = nullptr;
	HashMap<String, CheckBox *> category_checks;
	ItemList *sessions_list = nullptr;
	RichTextLabel *activity_log = nullptr;

	bool start_attempted = false;
	bool updating_ui = false;
	int retry_attempts_left = 0;
	uint64_t next_retry_msec = 0;

	void _build_dock();
	void _rebuild_category_toggles();
	void _apply_categories_from_settings();
	void _save_categories_to_settings();
	void _refresh_ui();
	void _refresh_sessions();
	void _refresh_activity();
	void _attempt_auto_start();
	void _start_server_from_settings();
	void _on_toggle_server();
	void _on_restart_server();
	void _on_port_changed(double p_value);
	void _on_auto_start_toggled(bool p_pressed);
	void _on_category_toggled(bool p_pressed, const String &p_category);
	void _on_reset_all_categories();
	void _on_copy_url();
	void _on_copy_mcp_config();
	void _on_clear_activity();
	void _on_server_status_changed();
	void _on_server_sessions_changed();
	void _on_tool_executed(const String &p_tool_name, bool p_ok, const String &p_summary);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	MCPEditorPlugin();
};

// Lightweight lifecycle host attached to ProjectManager so MCP is available
// even before opening a project (e.g. for project_create / project_open).
class MCPProjectManagerHost : public Node {
	GDCLASS(MCPProjectManagerHost, Node);

	bool started = false;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	MCPProjectManagerHost();
};

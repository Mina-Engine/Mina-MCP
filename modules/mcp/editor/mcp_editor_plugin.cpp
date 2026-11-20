/**************************************************************************/
/*  mcp_editor_plugin.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_editor_plugin.h"

#include "../mcp_server.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/docks/editor_dock.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "servers/display/display_server.h"

namespace {

int _resolve_configured_port() {
	if (MinaMCPServer::port_override > 0) {
		return MinaMCPServer::port_override;
	}
	String env_port = OS::get_singleton()->get_environment("MINA_MCP_PORT");
	if (!env_port.is_empty()) {
		int p = env_port.to_int();
		if (p >= 1 && p <= 65535) {
			return p;
		}
	}
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting("network/mcp_server/port")) {
		return int(EditorSettings::get_singleton()->get_setting("network/mcp_server/port"));
	}
	return MinaMCPServer::DEFAULT_PORT;
}

String _resolve_configured_host() {
	String env_host = OS::get_singleton()->get_environment("MINA_MCP_HOST");
	if (!env_host.is_empty()) {
		return env_host;
	}
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting("network/mcp_server/host")) {
		return String(EditorSettings::get_singleton()->get_setting("network/mcp_server/host"));
	}
	return "127.0.0.1";
}

bool _resolve_auto_start() {
	if (MinaMCPServer::disabled_override) {
		return false;
	}
	String env_disable = OS::get_singleton()->get_environment("MINA_MCP_DISABLE");
	if (env_disable == "1" || env_disable.to_lower() == "true") {
		return false;
	}
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting("network/mcp_server/auto_start")) {
		return bool(EditorSettings::get_singleton()->get_setting("network/mcp_server/auto_start"));
	}
	return true;
}

bool _resolve_allow_remote() {
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting("network/mcp_server/allow_remote")) {
		return bool(EditorSettings::get_singleton()->get_setting("network/mcp_server/allow_remote"));
	}
	return false;
}

} // namespace

MCPEditorPlugin::MCPEditorPlugin() {
	set_process_internal(true);
}

void MCPEditorPlugin::_build_dock() {
	dock = memnew(EditorDock);
	dock->set_name("MinaMCPServerDock");
	dock->set_title("MCP Server");
	dock->set_layout_key("mina_mcp_server");
	dock->set_icon_name("MinaEngine");
	dock->set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BR);
	dock->set_custom_minimum_size(Size2(300, 0) * EDSCALE);

	VBoxContainer *root = memnew(VBoxContainer);
	root->add_theme_constant_override("separation", 6 * EDSCALE);
	dock->add_child(root);

	// Status row.
	HBoxContainer *status_row = memnew(HBoxContainer);
	Label *status_prefix = memnew(Label);
	status_prefix->set_text(String::utf8("状態:"));
	status_row->add_child(status_prefix);
	status_label = memnew(Label);
	status_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	status_row->add_child(status_label);
	toggle_button = memnew(Button);
	toggle_button->connect(SceneStringName(pressed), callable_mp(this, &MCPEditorPlugin::_on_toggle_server));
	status_row->add_child(toggle_button);
	restart_button = memnew(Button);
	restart_button->set_text(String::utf8("再起動"));
	restart_button->connect(SceneStringName(pressed), callable_mp(this, &MCPEditorPlugin::_on_restart_server));
	status_row->add_child(restart_button);
	root->add_child(status_row);

	error_label = memnew(Label);
	error_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	error_label->set_visible(false);
	error_label->add_theme_color_override("font_color", Color(1.0, 0.45, 0.45));
	root->add_child(error_label);

	// Endpoint row.
	HBoxContainer *endpoint_row = memnew(HBoxContainer);
	endpoint_field = memnew(LineEdit);
	endpoint_field->set_editable(false);
	endpoint_field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	endpoint_row->add_child(endpoint_field);
	Button *copy_url_btn = memnew(Button);
	copy_url_btn->set_text(String::utf8("URLコピー"));
	copy_url_btn->set_tooltip_text(String::utf8("MCP エンドポイント URL をクリップボードにコピーします。"));
	copy_url_btn->connect(SceneStringName(pressed), callable_mp(this, &MCPEditorPlugin::_on_copy_url));
	endpoint_row->add_child(copy_url_btn);
	Button *copy_config_btn = memnew(Button);
	copy_config_btn->set_text(String::utf8("設定JSON"));
	copy_config_btn->set_tooltip_text(String::utf8("汎用 MCP クライアント用の JSON 設定をクリップボードにコピーします。"));
	copy_config_btn->connect(SceneStringName(pressed), callable_mp(this, &MCPEditorPlugin::_on_copy_mcp_config));
	endpoint_row->add_child(copy_config_btn);
	root->add_child(endpoint_row);

	// Port & Auto-start controls.
	HBoxContainer *config_row = memnew(HBoxContainer);
	Label *port_lbl = memnew(Label);
	port_lbl->set_text(String::utf8("ポート:"));
	config_row->add_child(port_lbl);
	port_spin = memnew(SpinBox);
	port_spin->set_min(1024);
	port_spin->set_max(65535);
	port_spin->set_step(1);
	port_spin->set_value(_resolve_configured_port());
	port_spin->set_editable(MinaMCPServer::port_override <= 0);
	port_spin->connect("value_changed", callable_mp(this, &MCPEditorPlugin::_on_port_changed));
	config_row->add_child(port_spin);

	auto_start_check = memnew(CheckBox);
	auto_start_check->set_text(String::utf8("自動起動"));
	auto_start_check->set_pressed(_resolve_auto_start());
	auto_start_check->connect(SceneStringName(toggled), callable_mp(this, &MCPEditorPlugin::_on_auto_start_toggled));
	config_row->add_child(auto_start_check);
	root->add_child(config_row);

	root->add_child(memnew(HSeparator));

	stats_label = memnew(Label);
	stats_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	root->add_child(stats_label);

	Label *sessions_title = memnew(Label);
	sessions_title->set_text(String::utf8("接続中のクライアント"));
	root->add_child(sessions_title);

	sessions_list = memnew(ItemList);
	sessions_list->set_custom_minimum_size(Size2(0, 72) * EDSCALE);
	root->add_child(sessions_list);

	HBoxContainer *activity_header = memnew(HBoxContainer);
	Label *activity_title = memnew(Label);
	activity_title->set_text(String::utf8("MCP 実行ログ"));
	activity_title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	activity_header->add_child(activity_title);
	Button *clear_activity_btn = memnew(Button);
	clear_activity_btn->set_text(String::utf8("クリア"));
	clear_activity_btn->connect(SceneStringName(pressed), callable_mp(this, &MCPEditorPlugin::_on_clear_activity));
	activity_header->add_child(clear_activity_btn);
	root->add_child(activity_header);

	activity_log = memnew(RichTextLabel);
	activity_log->set_use_bbcode(false);
	activity_log->set_scroll_follow(true);
	activity_log->set_selection_enabled(true);
	activity_log->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	activity_log->set_custom_minimum_size(Size2(0, 140) * EDSCALE);
	root->add_child(activity_log);

	EditorDockManager::get_singleton()->add_dock(dock);
	_refresh_ui();
}

void MCPEditorPlugin::_attempt_auto_start() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!server) {
		return;
	}
	server->set_mode_name("editor");
	server->invoke_provider_callbacks();

	if (!_resolve_auto_start()) {
		_refresh_ui();
		return;
	}
	_start_server_from_settings();
}

void MCPEditorPlugin::_start_server_from_settings() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!server) {
		return;
	}
	int port = _resolve_configured_port();
	String host = _resolve_configured_host();
	bool allow_remote = _resolve_allow_remote();

	Error err = server->start(port, host, allow_remote);
	if (err == OK) {
		retry_attempts_left = 0;
		if (EditorNode::get_log()) {
			EditorNode::get_log()->add_message(vformat("--- Mina MCP server started on %s ---", server->get_endpoint_url()), EditorLog::MSG_TYPE_EDITOR);
		}
	} else if (err == ERR_ALREADY_IN_USE && retry_attempts_left > 0) {
		// Brief handoff retry (e.g. Project Manager just closed and is releasing the port).
		next_retry_msec = OS::get_singleton()->get_ticks_msec() + 250;
	} else {
		retry_attempts_left = 0;
		if (EditorNode::get_log()) {
			EditorNode::get_log()->add_message(vformat("--- Mina MCP server failed to start (engine remains usable): %s ---", server->get_last_error()), EditorLog::MSG_TYPE_WARNING);
		}
	}
	_refresh_ui();
}

void MCPEditorPlugin::_refresh_ui() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!dock || !server) {
		return;
	}
	updating_ui = true;
	MinaMCPServer::ServerState st = server->get_state();
	if (st == MinaMCPServer::STATE_RUNNING) {
		status_label->set_text(String::utf8("稼働中 (ローカル専用)"));
		status_label->add_theme_color_override("font_color", Color(0.45, 0.92, 0.55));
		toggle_button->set_text(String::utf8("停止"));
		error_label->set_visible(false);
	} else if (st == MinaMCPServer::STATE_ERROR) {
		status_label->set_text(String::utf8("起動エラー"));
		status_label->add_theme_color_override("font_color", Color(1.0, 0.45, 0.45));
		toggle_button->set_text(String::utf8("起動"));
		error_label->set_text(server->get_last_error());
		error_label->set_visible(!server->get_last_error().is_empty());
	} else {
		status_label->set_text(String::utf8("停止中"));
		status_label->add_theme_color_override("font_color", Color(0.75, 0.75, 0.75));
		toggle_button->set_text(String::utf8("起動"));
		error_label->set_visible(false);
	}

	endpoint_field->set_text(server->get_endpoint_url());
	port_spin->set_value(server->get_port());
	auto_start_check->set_pressed(_resolve_auto_start());
	stats_label->set_text(vformat(String::utf8("公開ツール: %d  |  リソース: %d  |  累計リクエスト: %d"),
			server->get_tool_count(), server->get_resource_count(), (int64_t)server->get_total_requests()));

	updating_ui = false;
	_refresh_sessions();
	_refresh_activity();
}

void MCPEditorPlugin::_refresh_sessions() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!sessions_list || !server) {
		return;
	}
	sessions_list->clear();
	Array sessions = server->get_sessions();
	if (sessions.is_empty()) {
		sessions_list->add_item(String::utf8("（未接続 — 外部エージェントからの接続待機中）"));
		return;
	}
	for (int i = 0; i < sessions.size(); i++) {
		Dictionary s = sessions[i];
		String name = s.get("client_name", "client");
		String ver = s.get("client_version", "");
		String proto = s.get("protocol_version", "");
		int64_t reqs = s.get("request_count", 0);
		String label = ver.is_empty()
				? vformat("%s [%s] (%d req)", name, proto, reqs)
				: vformat("%s %s [%s] (%d req)", name, ver, proto, reqs);
		sessions_list->add_item(label);
	}
}

void MCPEditorPlugin::_refresh_activity() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!activity_log || !server) {
		return;
	}
	Array entries = server->get_recent_activity(40);
	String text;
	for (int i = 0; i < entries.size(); i++) {
		Dictionary e = entries[i];
		bool ok = e.get("ok", true);
		String tool = e.get("tool_name", "");
		double ms = e.get("duration_ms", 0.0);
		String summary = e.get("summary", "");
		text += vformat("[%s] %s (%.1f ms)%s\n",
				ok ? "OK" : "ERR",
				tool,
				ms,
				ok ? String() : (String(" - ") + summary));
	}
	activity_log->set_text(text);
}

void MCPEditorPlugin::_on_toggle_server() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!server) {
		return;
	}
	if (server->is_running()) {
		server->stop();
	} else {
		retry_attempts_left = 0;
		_start_server_from_settings();
	}
}

void MCPEditorPlugin::_on_restart_server() {
	retry_attempts_left = 0;
	_start_server_from_settings();
}

void MCPEditorPlugin::_on_port_changed(double p_value) {
	if (updating_ui || MinaMCPServer::port_override > 0) {
		return;
	}
	int new_port = int(p_value);
	if (EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_setting("network/mcp_server/port", new_port);
		EditorSettings::get_singleton()->save();
	}
}

void MCPEditorPlugin::_on_auto_start_toggled(bool p_pressed) {
	if (updating_ui) {
		return;
	}
	if (EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_setting("network/mcp_server/auto_start", p_pressed);
		EditorSettings::get_singleton()->save();
	}
}

void MCPEditorPlugin::_on_copy_url() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (server && DisplayServer::get_singleton()) {
		DisplayServer::get_singleton()->clipboard_set(server->get_endpoint_url());
	}
}

void MCPEditorPlugin::_on_copy_mcp_config() {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (server && DisplayServer::get_singleton()) {
		String json = vformat("{\n  \"mcpServers\": {\n    \"mina-engine\": {\n      \"url\": \"%s\"\n    }\n  }\n}\n", server->get_endpoint_url());
		DisplayServer::get_singleton()->clipboard_set(json);
	}
}

void MCPEditorPlugin::_on_clear_activity() {
	if (MinaMCPServer::get_singleton()) {
		MinaMCPServer::get_singleton()->clear_activity();
		_refresh_activity();
	}
}

void MCPEditorPlugin::_on_server_status_changed() {
	_refresh_ui();
}

void MCPEditorPlugin::_on_server_sessions_changed() {
	_refresh_sessions();
}

void MCPEditorPlugin::_on_tool_executed(const String &p_tool_name, bool p_ok, const String &p_summary) {
	_refresh_ui();
}

void MCPEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (server) {
				server->connect(SNAME("status_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_status_changed));
				server->connect(SNAME("sessions_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_sessions_changed));
				server->connect(SNAME("tool_executed"), callable_mp(this, &MCPEditorPlugin::_on_tool_executed));
			}
			_build_dock();
			retry_attempts_left = 8; // ~2 seconds grace for ProjectManager -> Editor port handoff.
		} break;

		case NOTIFICATION_EXIT_TREE: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (server) {
				if (server->is_connected(SNAME("status_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_status_changed))) {
					server->disconnect(SNAME("status_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_status_changed));
				}
				if (server->is_connected(SNAME("sessions_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_sessions_changed))) {
					server->disconnect(SNAME("sessions_changed"), callable_mp(this, &MCPEditorPlugin::_on_server_sessions_changed));
				}
				if (server->is_connected(SNAME("tool_executed"), callable_mp(this, &MCPEditorPlugin::_on_tool_executed))) {
					server->disconnect(SNAME("tool_executed"), callable_mp(this, &MCPEditorPlugin::_on_tool_executed));
				}
				server->stop();
			}
			if (dock) {
				EditorDockManager::get_singleton()->remove_dock(dock);
				memdelete(dock);
				dock = nullptr;
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (!server) {
				break;
			}
			if (!start_attempted && EditorNode::get_singleton() && EditorNode::get_singleton()->is_editor_ready()) {
				start_attempted = true;
				_attempt_auto_start();
			} else if (retry_attempts_left > 0 && !server->is_running() && OS::get_singleton()->get_ticks_msec() >= next_retry_msec) {
				retry_attempts_left--;
				_start_server_from_settings();
			}

			if (server->is_running()) {
				server->poll();
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (!EditorSettings::get_singleton()->check_changed_settings_in_group("network/mcp_server")) {
				break;
			}
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (!server) {
				break;
			}
			int cfg_port = _resolve_configured_port();
			String cfg_host = _resolve_configured_host();
			bool cfg_remote = _resolve_allow_remote();
			if (server->is_running() && (cfg_port != server->get_port() || cfg_host != server->get_host() || cfg_remote != server->is_remote_allowed())) {
				retry_attempts_left = 0;
				_start_server_from_settings();
			} else {
				_refresh_ui();
			}
		} break;
	}
}

MCPProjectManagerHost::MCPProjectManagerHost() {
	set_process_internal(true);
}

void MCPProjectManagerHost::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (!server || !_resolve_auto_start()) {
				break;
			}
			server->set_mode_name("project_manager");
			server->invoke_provider_callbacks();
			Error err = server->start(_resolve_configured_port(), _resolve_configured_host(), _resolve_allow_remote());
			started = (err == OK);
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (started && server && server->is_running()) {
				server->poll();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			MinaMCPServer *server = MinaMCPServer::get_singleton();
			if (started && server) {
				server->stop();
				started = false;
			}
		} break;
	}
}

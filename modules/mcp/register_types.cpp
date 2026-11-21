/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "register_types.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "mcp_runtime_bridge.h"
#include "mcp_server.h"

#ifdef TOOLS_ENABLED
#include "editor/mcp_builtin_tools.h"
#include "editor/mcp_editor_plugin.h"
#include "editor/mcp_runtime_debugger_plugin.h"
#include "editor/plugins/editor_plugin.h"
#endif

static MinaMCPServer *mcp_server_singleton = nullptr;

void initialize_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		GDREGISTER_CLASS(MinaMCPServer);
		mcp_server_singleton = memnew(MinaMCPServer);
		Engine::get_singleton()->add_singleton(Engine::Singleton("MinaMCPServer", mcp_server_singleton));
	}
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_INTERNAL_CLASS(MCPRuntimeInputDriver);
		MCPRuntimeBridge::initialize();
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(MCPProjectManagerHost);
		GDREGISTER_INTERNAL_CLASS(MCPRuntimeDebuggerPlugin);
		if (mcp_server_singleton) {
			mcp_register_builtin_tools(mcp_server_singleton);
		}
		EditorPlugins::add_by_type<MCPEditorPlugin>();
	}
#endif
}

void uninitialize_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		MCPRuntimeBridge::deinitialize();
	}
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		if (mcp_server_singleton) {
			mcp_server_singleton->stop();
			Engine::get_singleton()->remove_singleton("MinaMCPServer");
			memdelete(mcp_server_singleton);
			mcp_server_singleton = nullptr;
		}
	}
}

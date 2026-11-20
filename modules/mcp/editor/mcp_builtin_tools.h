/**************************************************************************/
/*  mcp_builtin_tools.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

class MinaMCPServer;

void mcp_register_project_tools(MinaMCPServer *p_server);
void mcp_register_scene_tools(MinaMCPServer *p_server);
void mcp_register_assets_scripts_tools(MinaMCPServer *p_server);
void mcp_register_animation_lighting_tools(MinaMCPServer *p_server);
void mcp_register_runtime_export_tools(MinaMCPServer *p_server);

inline void mcp_register_builtin_tools(MinaMCPServer *p_server) {
	mcp_register_project_tools(p_server);
	mcp_register_scene_tools(p_server);
	mcp_register_assets_scripts_tools(p_server);
	mcp_register_animation_lighting_tools(p_server);
	mcp_register_runtime_export_tools(p_server);
}

/**************************************************************************/
/*  horror_mcp_tools.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MCP_ENABLED
class MinaMCPServer;
void horror_register_mcp_tools(MinaMCPServer *p_server);
#endif

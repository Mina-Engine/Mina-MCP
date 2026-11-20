/**************************************************************************/
/*  mcp_variant_codec.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

namespace mcp {

// JSON Schema builder helpers for clean, declarative tool definitions.
Dictionary schema_string(const String &p_description, const Array &p_enum = Array());
Dictionary schema_int(const String &p_description);
Dictionary schema_number(const String &p_description);
Dictionary schema_bool(const String &p_description);
Dictionary schema_array(const String &p_description, const Dictionary &p_items = Dictionary());
Dictionary schema_object(const String &p_description, const Dictionary &p_properties = Dictionary(), const Array &p_required = Array(), bool p_additional_properties = true);
Dictionary schema_any(const String &p_description);
Dictionary make_tool_schema(const Dictionary &p_properties, const Array &p_required = Array());

// Tool response builders.
Dictionary make_ok(const Dictionary &p_data = Dictionary());
Dictionary make_error(const String &p_message, const String &p_code = "tool_error", const Dictionary &p_details = Dictionary());
Dictionary make_image_result(const String &p_base64_png, const Dictionary &p_metadata = Dictionary());

// Bidirectional Variant <-> JSON conversion (Vector2/3/4, Color, Quaternion, Basis, Transform2D/3D, Rect2, AABB, NodePath, Resource, etc.).
Variant variant_to_json(const Variant &p_value, int p_depth = 0);
Variant json_to_variant(const Variant &p_json, Variant::Type p_hint_type = Variant::NIL, String *r_error = nullptr);
bool parse_vector2(const Variant &p_value, Vector2 &r_out);
bool parse_vector3(const Variant &p_value, Vector3 &r_out);
bool parse_color(const Variant &p_value, Color &r_out);

// Safe path validation for project paths (res://) and absolute filesystem paths.
bool is_safe_res_path(const String &p_path, bool p_allow_root = false);
String normalize_res_path(const String &p_path);

} // namespace mcp

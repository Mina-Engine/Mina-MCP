/**************************************************************************/
/*  mcp_runtime_bridge.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "core/input/input_enums.h"
#include "core/os/keyboard.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class InputEventWithModifiers;

// Runs inside the target game process (only when EngineDebugger is active).
// Receives ordered input commands and screenshot requests over the private
// Editor <-> Game debugger connection and dispatches inputs exclusively via
// Godot's Input singleton (never global OS input).
class MCPRuntimeInputDriver : public Node {
	GDCLASS(MCPRuntimeInputDriver, Node);

	struct InputStep {
		String type; // key, action, mouse_move, mouse_button, mouse_scroll, gamepad_button, gamepad_axis, wait, release_all
		String mode; // tap, press, release, hold, click, double_click
		Vector<Key> keys;
		StringName action_name;
		float strength = 1.0f;
		MouseButton mouse_button = MouseButton::LEFT;
		bool has_position = false;
		Vector2 position;
		bool has_relative = false;
		Vector2 relative;
		int move_steps = 1;
		int scroll_steps = 1;
		float scroll_factor = 1.0f;
		int device = 0;
		JoyButton joy_button = JoyButton::INVALID;
		JoyAxis joy_axis = JoyAxis::INVALID;
		float axis_value = 0.0f;
		bool has_stick = false;
		JoyAxis stick_x_axis = JoyAxis::LEFT_X;
		JoyAxis stick_y_axis = JoyAxis::LEFT_Y;
		Vector2 stick_value;
		double duration_sec = 0.0;
	};

	struct QueuedRequest {
		int64_t request_id = 0;
		Vector<InputStep> steps;
		int current_index = 0;
		bool step_started = false;
		uint64_t step_start_msec = 0;
		uint64_t step_end_msec = 0;
		int move_steps_done = 0;
		Vector2 move_accum;
		int executed_steps = 0;
		bool ack_on_complete = true;
	};

	static MCPRuntimeInputDriver *singleton;

	Vector<QueuedRequest> queue;
	Array runtime_events;
	uint64_t next_runtime_event_id = 1;
	ObjectID last_scene_id;
	int last_runtime_node_count = -1;
	bool event_baseline_initialized = false;
	HashSet<Key> held_keys;
	HashSet<MouseButton> held_mouse_buttons;
	HashSet<StringName> held_actions;
	HashSet<uint64_t> held_joy_buttons;
	HashMap<uint64_t, float> held_joy_axes;
	HashSet<int> connected_virtual_joypads;
	Vector2 virtual_mouse_pos = Vector2(-1, -1);

	void _ensure_virtual_mouse_pos();
	void _ensure_virtual_joypad(int p_device);
	BitField<MouseButtonMask> _current_mouse_mask() const;
	void _apply_modifiers(InputEventWithModifiers *p_ev) const;

	void _dispatch_key(Key p_key, bool p_pressed);
	void _dispatch_action(const StringName &p_action, bool p_pressed, float p_strength = 1.0f);
	void _dispatch_mouse_move_relative(const Vector2 &p_delta);
	void _dispatch_mouse_move_absolute(const Vector2 &p_pos);
	void _dispatch_mouse_button(MouseButton p_btn, bool p_pressed, bool p_double_click = false, float p_factor = 1.0f);
	void _dispatch_joy_button(int p_device, JoyButton p_btn, bool p_pressed);
	void _dispatch_joy_axis(int p_device, JoyAxis p_axis, float p_value);

	bool _parse_step(const Dictionary &p_dict, InputStep &r_step, String &r_error) const;
	void _begin_step(QueuedRequest &p_req, InputStep &p_step, uint64_t p_now);
	void _finish_step(InputStep &p_step);
	void _tick_queue();
	void _sample_runtime_events();
	void _send_ack(int64_t p_request_id, bool p_ok, int p_executed_steps, const String &p_error = String());

	static Key _parse_key_name(const String &p_name);
	static MouseButton _parse_mouse_button(const String &p_name);
	static JoyButton _parse_joy_button(const Variant &p_val);
	static JoyAxis _parse_joy_axis(const String &p_name);
	static uint64_t _pack_joy_key(int p_device, int p_code) { return (uint64_t(uint32_t(p_device)) << 32) | uint64_t(uint32_t(p_code)); }

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	static MCPRuntimeInputDriver *get_or_create();
	static MCPRuntimeInputDriver *get_singleton() { return singleton; }

	void enqueue_command(int64_t p_request_id, const Dictionary &p_payload, bool p_ack_on_complete);
	Array get_runtime_events() const { return runtime_events.duplicate(true); }
	void release_all_inputs(bool p_cancel_queue = true);

	MCPRuntimeInputDriver();
	~MCPRuntimeInputDriver();
};

class MCPRuntimeBridge {
	static bool initialized;

	static Error _handle_capture_runtime(const Array &p_args);
	static Error _handle_send_input(const Array &p_args);
	static Error _handle_release_all(const Array &p_args);
	static Error _handle_runtime_request(const Array &p_args);

public:
	static void initialize();
	static void deinitialize();
	static Error parse_message(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured);
};

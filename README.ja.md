# Mina-MCP

[English README (README.md)](README.md)

**Mina-MCP** は、[Mina Engine](https://github.com/Mina-Engine/Mina-Engine)（Godot 4.7.2 ベース）向けに設計・実装された **組み込み MCP（Model Context Protocol）サーバーモジュール** です。

ゲームエンジンの起動と同時に（**Project Manager モード / Editor モードの両方で**）自動起動し、終了時に自動停止します。GUI の自動クリックではなくエンジン内部 C++ API を直接 MCP ツール・リソースとして公開するため、**Claude Code** や **Codex** などの外部 AI エージェントが、プロジェクト作成からシーン構築、ノード配置、アセット管理、スクリプト編集、アニメーション、ライティング、ゲーム実行、デバッグ、ビルド・エクスポートまでを一貫して操作できます。

---

## 主な仕様・特徴

- **エンジンライフサイクルとの完全連動**
  - エンジン起動時に MCP サーバーを自動起動（Project Manager / Editor 両対応）。
  - エンジン終了時にソケット・セッションを安全に自動停止。
  - ポート競合等で MCP 起動に失敗しても警告のみ記録し、エンジン本体は正常に使用可能。
- **標準 MCP トランスポート対応（外部依存なし）**
  - **Streamable HTTP**（`POST /mcp`, `GET /mcp`, `DELETE /mcp`）— MCP `2025-06-18` / `2025-03-26`
  - **レガシー HTTP+SSE**（`GET /sse`, `POST /messages?sessionId=...`）— MCP `2024-11-05`
  - **ヘルスチェック**（`GET /health`）
- **ローカル接続標準のセキュリティ設計**
  - デフォルトで `127.0.0.1:6800` のみにバインド。
  - `Host` / `Origin` ヘッダーを検証し、不用意な外部公開や DNS リバインディングを防止。
- **エディタ内管理ドック（`MCP Server`）**
  - 稼働状況、起動・停止・再起動、ポート番号・自動起動設定、**カテゴリ別ツール公開切替（`公開カテゴリ`）**、接続中セッション一覧、エンドポイント URL・汎用 MCP 設定 JSON のワンクリックコピー、リアルタイム実行ログを提供。
- **カテゴリ別ツール公開制御（コンテキスト圧迫防止）**
  - `scene` / `nodes` / `runtime` / `animation` / `lighting` / `export` など、作業に必要なカテゴリだけを AI に公開可能。
  - `MCP Server` ドックのチェックボックス、`EditorSettings`（`network/mcp_server/enabled_categories`）、動的ツール（`mcp_list_categories` / `mcp_set_enabled_categories`）、およびセッション単位の URL クエリ（例: `http://127.0.0.1:6800/mcp?categories=scene,nodes,runtime`）に対応。
- **高い拡張性**
  - C++ モジュールは `MinaMCPServer::add_provider_callback(...)` でコアコードを変更せずツールを追加可能。
  - GDScript / GDExtension からは `MinaMCPServer.register_script_tool(...)` / `register_script_resource(...)` で動的登録可能。

---

## MCP クライアントからの接続方法

任意の MCP 対応クライアントから `http://127.0.0.1:6800/mcp`（Streamable HTTP）または `http://127.0.0.1:6800/sse`（SSE）へ接続できます。

### 汎用 MCP 設定（JSON）

```json
{
  "mcpServers": {
    "mina-engine": {
      "url": "http://127.0.0.1:6800/mcp"
    }
  }
}
```

### 接続例（CLI / TOML クライアント）

```bash
# 例: Claude Code
claude mcp add --transport http mina-engine http://127.0.0.1:6800/mcp
```

```toml
# 例: Codex (~/.codex/config.toml)
[mcp_servers.mina-engine]
url = "http://127.0.0.1:6800/mcp"
```

---

## 設定・CLI・環境変数

- **エディター設定**: `network/mcp_server/auto_start`, `network/mcp_server/host`, `network/mcp_server/port`, `network/mcp_server/allow_remote`, `network/mcp_server/enabled_categories`
- **CLI オプション**: `--mcp-port <port>`, `--no-mcp`
- **環境変数**: `MINA_MCP_PORT`, `MINA_MCP_HOST`, `MINA_MCP_DISABLE=1`

---

## 公開ツール・リソース一覧

- **ライブ MCP リソース（5種）**: `mina://engine/status`, `mina://project/info`, `mina://scene/current`, `mina://logs/recent`, `mina://debug/runtime_errors`
- **エンジン・プロジェクト（12）**: `engine_get_status`, `mcp_list_categories`, `mcp_set_enabled_categories`, `project_create`, `project_list`, `project_open`, `project_get_info`, `project_get_setting`, `project_set_setting`, `project_list_settings`, `project_configure_input_action`, `project_manage_autoload`
- **シーン・ノード・エディタ操作（30）**: `scene_list_open`, `scene_create`, `scene_open`, `scene_save`, `scene_close`, `scene_get_tree`, `scene_find_nodes`（名前・クラス・グループ・スクリプト・プロパティ条件検索）, `scene_diff`（保存済み `.tscn` 等との差分比較）, `scene_create_inherited`（継承シーン作成）, `scene_pack_selection`（選択ノードの再利用シーン化）, `scene_instantiate`, `node_create`, `node_delete`, `node_duplicate`, `node_reparent`, `node_rename`, `node_get_properties`, `node_set_properties`, `node_set_transform`, `node_get_bounds`（3D AABB / 2D Rect2 境界ボックス取得）, `node_align`（複数ノードの整列・等間隔配置）, `node_snap_to_surface`（床・壁・天井へのスナップ配置）, `node_manage_groups`, `node_connect_signal`, `node_disconnect_signal`, `node_list_signals`, `node_call_method`, `editor_select_nodes`, `editor_undo`, `editor_redo`
- **アセット・スクリプト（17）**: `asset_list_files`, `asset_get_info`, `asset_create_resource`, `asset_inspect_resource`, `asset_update_resource`, `asset_create_directory`, `asset_move_file`, `asset_copy_file`, `asset_delete_file`, `asset_scan_filesystem`, `asset_reimport`, `script_read`, `script_write`, `script_patch`, `script_attach`, `script_validate`, `script_open_in_editor`
- **フリーアセット取得（2・CC0）**: `asset_search_free`, `asset_download_free`（Poly Haven / ambientCG からPBRテクスチャ・3Dモデル・HDRIを検索し `res://assets/free/<id>/` にダウンロード。HTTPS・許可ホストのみ、`res://` 配下にのみ書込、`LICENSE_CC0.txt` を自動生成）
- **アニメーション・ライティング・型情報（11）**: `animation_list`, `animation_create`, `animation_delete`, `animation_add_track`, `animation_insert_key`, `animation_remove_key`, `animation_get_details`, `lighting_add_light`, `lighting_configure_environment`, `class_list`, `class_get_info`
- **ゲーム実行・入力操作・デバッグ・ビルド/エクスポート（15）**: `game_play`, `game_stop`, `game_get_status`, `game_capture_runtime`（実行中ゲーム画面のPNG撮影）, `game_send_input`（キーボード・同時押し・長押し・マウス相対/絶対移動・クリック・スクロール・ゲームパッド・順序保証シーケンス・全解除／対象ゲームプロセス限定）, `debug_get_logs`, `debug_clear_logs`, `debug_get_runtime_errors`, `debug_clear_runtime_errors`, `debug_set_breakpoint`, `debug_capture_viewport`, `export_list_platforms`, `export_list_presets`, `export_create_preset`, `export_project`
- **拡張実装例（`examples/horror_mcp_tools/`）**: `horror_get_director_state`, `horror_set_director_state`, `horror_create_interactable`

---

## エンジンへの導入方法

1. `modules/mcp/` をエンジンソースの `modules/mcp/` に配置します（または SCons の `custom_modules` で指定します）。
2. （推奨）`integration/engine_integration.patch` を適用すると、`EditorSettings` の初期値、`--mcp-port` / `--no-mcp` CLI 引数、および Project Manager モードでの自動起動が有効になります。
3. 通常通り SCons でエディタをビルドします。

---

## テスト

```bash
python tests/test_mcp_e2e.py --engine /path/to/Mina\ Engine.exe
```

---

## ライセンス

MIT License — 詳細は [LICENSE](LICENSE) を参照してください。


#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <nlohmann/json.hpp>
#include "key_manager.h"

// ============================================================================
// Config & Path Helpers
// ============================================================================

bool file_exists(const std::string& path);
std::string get_home_dir();
std::string find_config_file();
nlohmann::json parse_jsonc(const std::string& filepath);
void update_opencode_config(const std::vector<std::string>& models_list);

// ============================================================================
// Config Sync
// ============================================================================

void run_sync_config_task(KeyManager& key_manager, std::atomic<bool>& shutdown);

// ============================================================================
// Token Estimation & Model Mapping
// ============================================================================

int estimate_input_tokens(const nlohmann::json& anthropic_json);
std::string map_anthropic_model_to_nim(const std::string& anthropic_model);
int get_model_context_window(const std::string& model_id);

// ============================================================================
// API Key Loading
// ============================================================================

std::vector<std::string> load_api_keys();

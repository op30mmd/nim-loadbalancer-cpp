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
std::vector<std::string> load_provider_keys(const std::string& type, const std::vector<std::string>& fallback_keys);

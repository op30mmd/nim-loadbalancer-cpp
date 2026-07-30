#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "proxy_config.h"
#include "utils.h"

// ============================================================================
// Anthropic Compatibility Translation
// ============================================================================

std::string process_line_for_anthropic_stream(const std::string& line, AnthropicStreamState& state);
nlohmann::json convert_anthropic_to_openai_request(const nlohmann::json& anthropic_json);
nlohmann::json convert_openai_to_anthropic_response(const nlohmann::json& openai_json, const std::string& fallback_model);
std::string apply_compatibility_layer(const std::string& body_str, const std::string& original_model = "");

// ============================================================================
// Anthropic Compatibility Helpers
// ============================================================================

std::string gen_request_id();
nlohmann::json make_anthropic_error(const std::string& type, const std::string& message);

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "proxy_config.h"
#include "utils.h"

// ============================================================================
// Anthropic Compatibility Translation
// ============================================================================

struct AnthropicStreamState {
	bool message_started = false;
	bool text_block_started = false;
	bool thinking_block_started = false;
	bool thinking_started = false;
	bool thinking_finished = false;

	bool tool_block_started = false;
	std::string current_tool_id = "";
	std::string current_tool_name = "";

	int current_block_index = -1;
	std::string msg_id;
	std::string model = "nvidia-nim-model";
	int input_tokens = 0;
	int output_tokens = 0;
	bool message_stopped = false;
	std::string finish_reason = "end_turn";

	AnthropicStreamState() {
		msg_id = gen_random_id("msg_", 1000000);
	}
};

struct LambdaState {
	std::string buffer = "";
	AnthropicStreamState anthropic_state;
	size_t total_bytes = 0;
	size_t lines_emitted = 0;
	std::chrono::steady_clock::time_point stream_start = std::chrono::steady_clock::now();
	std::shared_ptr<std::thread> curl_thread;
	std::shared_ptr<ProxyContext> ctx;
	CURL* curl = nullptr;
	struct curl_slist* headers_list = nullptr;
	std::function<void()> on_destroy;

	~LambdaState() {
		if (ctx) {
			ctx->client_disconnected = true;
			ctx->chunk_queue.finish();
		}
		if (curl_thread && curl_thread->joinable()) {
			curl_thread->join();
		}
		if (curl) {
			curl_easy_cleanup(curl);
		}
		if (headers_list) {
			curl_slist_free_all(headers_list);
		}

		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - stream_start).count() / 1000.0;
		LOG_INFO("Stream", "Session terminated | Duration: " + std::to_string(duration) + "s | Lines: " + std::to_string(lines_emitted) + " | Size: " + std::to_string(total_bytes / 1024.0) + " KB");
		if (on_destroy) on_destroy();
	}
};

std::string process_line_for_anthropic_stream(const std::string& line, AnthropicStreamState& state);
nlohmann::json convert_anthropic_to_openai_request(const nlohmann::json& anthropic_json);
nlohmann::json convert_openai_to_anthropic_response(const nlohmann::json& openai_json, const std::string& fallback_model);
std::string apply_compatibility_layer(const std::string& body_str, const std::string& original_model = "");

// ============================================================================
// Anthropic Compatibility Helpers
// ============================================================================

std::string gen_request_id();
nlohmann::json make_anthropic_error(const std::string& type, const std::string& message);

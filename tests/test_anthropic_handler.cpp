#include <catch2/catch_all.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "anthropic_handler.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TEST_CASE("gen_request_id has the req_ prefix", "[anthropic]") {
	REQUIRE(gen_request_id().rfind("req_", 0) == 0);
	REQUIRE(gen_request_id().size() > 4);
}

TEST_CASE("make_anthropic_error builds the expected envelope", "[anthropic]") {
	auto err = make_anthropic_error("rate_limit_error", "Slow down");
	REQUIRE(err["type"] == "error");
	REQUIRE(err["error"]["type"] == "rate_limit_error");
	REQUIRE(err["error"]["message"] == "Slow down");
}

// ---------------------------------------------------------------------------
// convert_anthropic_to_openai_request
// ---------------------------------------------------------------------------

TEST_CASE("request conversion maps model, stream and max_tokens", "[anthropic]") {
	nlohmann::json in = {
		{"model", "claude-sonnet-4"},
		{"stream", true},
		{"max_tokens", 2048},
		{"messages", nlohmann::json::array()}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["model"] == "meta/llama-3.1-405b-instruct");
	REQUIRE(out["stream"] == true);
	REQUIRE(out["max_tokens"] == 2048);
	REQUIRE(out["messages"].is_array());
}

TEST_CASE("request conversion defaults stream to false", "[anthropic]") {
	nlohmann::json in = {{"messages", nlohmann::json::array()}};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["stream"] == false);
}

TEST_CASE("request conversion clamps temperature", "[anthropic]") {
	auto out = convert_anthropic_to_openai_request({
		{"temperature", 2.5},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE(out["temperature"] == 1.0);

	out = convert_anthropic_to_openai_request({
		{"temperature", -3.0},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE(out["temperature"] == 0.0);

	out = convert_anthropic_to_openai_request({
		{"temperature", 0.5},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE(out["temperature"] == 0.5);
}

TEST_CASE("request conversion passes top_p, top_k and stop sequences", "[anthropic]") {
	nlohmann::json in = {
		{"top_p", 0.9},
		{"top_k", 40},
		{"stop_sequences", {"\n\n", "END"}},
		{"messages", nlohmann::json::array()}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["top_p"] == 0.9);
	REQUIRE(out["top_k"] == 40);
	REQUIRE(out["stop"] == nlohmann::json::array({"\n\n", "END"}));
}

TEST_CASE("request conversion maps metadata user_id to user", "[anthropic]") {
	nlohmann::json in = {
		{"metadata", {{"user_id", "user-123"}}},
		{"messages", nlohmann::json::array()}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["user"] == "user-123");
}

TEST_CASE("request conversion handles string and block systems", "[anthropic]") {
	auto out = convert_anthropic_to_openai_request({
		{"system", "Be concise"},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE(out["messages"][0] == nlohmann::json({{"role", "system"}, {"content", "Be concise"}}));

	out = convert_anthropic_to_openai_request({
		{"system", nlohmann::json::array({
			{{"type", "text"}, {"text", "Part one"}},
			{{"type", "text"}, {"text", "Part two"}}
		})},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE(out["messages"][0]["content"] == "Part one\nPart two\n");
}

TEST_CASE("request conversion handles string message content", "[anthropic]") {
	auto out = convert_anthropic_to_openai_request({
		{"messages", nlohmann::json::array({
			{{"role", "user"}, {"content", "Hello there"}}
		})}
	});
	REQUIRE(out["messages"].size() == 1);
	REQUIRE(out["messages"][0]["role"] == "user");
	REQUIRE(out["messages"][0]["content"] == "Hello there");
}

TEST_CASE("request conversion aggregates text and tool blocks", "[anthropic]") {
	nlohmann::json in = {
		{"messages", nlohmann::json::array({
			{{"role", "assistant"}, {"content", nlohmann::json::array({
				{{"type", "text"}, {"text", "Let me check "}},
				{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "lookup"},
				 {"input", {{"q", "weather"}}}},
				{{"type", "text"}, {"text", "the weather"}}
			})}},
			{{"role", "user"}, {"content", nlohmann::json::array({
				{{"type", "tool_result"}, {"tool_use_id", "toolu_1"},
				 {"content", nlohmann::json::array({
					{{"type", "text"}, {"text", "sunny"}}
				 })}}
			})}}
		})}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["messages"].size() == 2);
	// Aggregated assistant message with text + tool_calls
	REQUIRE(out["messages"][0]["role"] == "assistant");
	REQUIRE(out["messages"][0]["content"] == "Let me check the weather");
	REQUIRE(out["messages"][0]["tool_calls"][0]["id"] == "toolu_1");
	REQUIRE(out["messages"][0]["tool_calls"][0]["function"]["name"] == "lookup");
	REQUIRE(out["messages"][0]["tool_calls"][0]["function"]["arguments"] == "{\"q\":\"weather\"}");
	// tool_result becomes a role=tool message
	REQUIRE(out["messages"][1]["role"] == "tool");
	REQUIRE(out["messages"][1]["tool_call_id"] == "toolu_1");
	REQUIRE(out["messages"][1]["content"] == "sunny");
	// The empty user wrapper is dropped (tool message carries the result)
}

TEST_CASE("request conversion maps tools with input_schema", "[anthropic]") {
	nlohmann::json in = {
		{"tools", nlohmann::json::array({
			{{"name", "lookup"},
			 {"description", "Look things up"},
			 {"input_schema", {{"type", "object"}}}}
		})},
		{"messages", nlohmann::json::array()}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["tools"][0]["type"] == "function");
	REQUIRE(out["tools"][0]["function"]["name"] == "lookup");
	REQUIRE(out["tools"][0]["function"]["description"] == "Look things up");
	REQUIRE(out["tools"][0]["function"]["parameters"] == nlohmann::json({{"type", "object"}}));
}

TEST_CASE("request conversion maps tool_choice variants", "[anthropic]") {
	auto base = [](const std::string& type, const std::string& name = "") {
		nlohmann::json tc = {{"type", type}};
		if (!name.empty()) tc["name"] = name;
		return nlohmann::json{{"tool_choice", tc}, {"messages", nlohmann::json::array()}};
	};

	auto out = convert_anthropic_to_openai_request(base("auto"));
	REQUIRE(out["tool_choice"] == "auto");

	out = convert_anthropic_to_openai_request(base("any"));
	REQUIRE(out["tool_choice"] == "required");

	out = convert_anthropic_to_openai_request(base("tool", "lookup"));
	REQUIRE(out["tool_choice"]["type"] == "function");
	REQUIRE(out["tool_choice"]["function"]["name"] == "lookup");

	// "none" erases tools + tool_choice
	out = convert_anthropic_to_openai_request({
		{"tool_choice", {{"type", "none"}}},
		{"tools", nlohmann::json::array({{"name", "x"}})},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE_FALSE(out.contains("tool_choice"));
	REQUIRE_FALSE(out.contains("tools"));
}

TEST_CASE("request conversion maps thinking to chat_template_kwargs", "[anthropic]") {
	nlohmann::json in = {
		{"thinking", {{"type", "enabled"}, {"budget_tokens", 4096}}},
		{"messages", nlohmann::json::array()}
	};
	auto out = convert_anthropic_to_openai_request(in);
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
	REQUIRE(out["reasoning_budget"] == 4096);

	// Disabled thinking adds nothing
	out = convert_anthropic_to_openai_request({
		{"thinking", {{"type", "disabled"}}},
		{"messages", nlohmann::json::array()}
	});
	REQUIRE_FALSE(out.contains("chat_template_kwargs"));
}

// ---------------------------------------------------------------------------
// convert_openai_to_anthropic_response
// ---------------------------------------------------------------------------

TEST_CASE("response conversion produces an Anthropic message", "[anthropic]") {
	nlohmann::json in = {
		{"id", "chatcmpl-42"},
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"}, {"content", "Hi there"}}},
			 {"finish_reason", "stop"}}
		})},
		{"usage", {{"prompt_tokens", 3}, {"completion_tokens", 5}}}
	};
	auto out = convert_openai_to_anthropic_response(in, "claude-sonnet-4");
	REQUIRE(out["id"] == "msg_chatcmpl-42");
	REQUIRE(out["type"] == "message");
	REQUIRE(out["role"] == "assistant");
	REQUIRE(out["model"] == "claude-sonnet-4");
	REQUIRE(out["content"].size() == 1);
	REQUIRE(out["content"][0]["type"] == "text");
	REQUIRE(out["content"][0]["text"] == "Hi there");
	REQUIRE(out["stop_reason"] == "end_turn");
	REQUIRE(out["usage"]["input_tokens"] == 3);
	REQUIRE(out["usage"]["output_tokens"] == 5);
}

TEST_CASE("response conversion maps finish reasons", "[anthropic]") {
	nlohmann::json tool_calls = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"}, {"content", nullptr}}},
			 {"finish_reason", "tool_calls"}}
		})},
		{"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}
	};
	REQUIRE(convert_openai_to_anthropic_response(tool_calls, "m")["stop_reason"] == "tool_use");

	nlohmann::json length = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"}, {"content", "x"}}},
			 {"finish_reason", "length"}}
		})},
		{"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}
	};
	REQUIRE(convert_openai_to_anthropic_response(length, "m")["stop_reason"] == "max_tokens");
}

TEST_CASE("response conversion emits thinking blocks", "[anthropic]") {
	nlohmann::json in = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"},
				{"reasoning_content", "Let me reason"},
				{"content", "Answer"}}},
			 {"finish_reason", "stop"}}
		})},
		{"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}
	};
	auto out = convert_openai_to_anthropic_response(in, "m");
	REQUIRE(out["content"].size() == 2);
	REQUIRE(out["content"][0]["type"] == "thinking");
	REQUIRE(out["content"][0]["thinking"] == "Let me reason");
	REQUIRE(out["content"][1]["type"] == "text");
}

TEST_CASE("response conversion emits tool_use blocks with parsed args", "[anthropic]") {
	nlohmann::json in = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"},
				{"content", nullptr},
				{"tool_calls", nlohmann::json::array({
					{{"id", "call_9"},
					 {"type", "function"},
					 {"function", {{"name", "lookup"}, {"arguments", "{\"q\":\"NYC\"}"}}}}
				})}}},
			 {"finish_reason", "tool_calls"}}
		})},
		{"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}
	};
	auto out = convert_openai_to_anthropic_response(in, "m");
	REQUIRE(out["content"].size() == 1);
	REQUIRE(out["content"][0]["type"] == "tool_use");
	REQUIRE(out["content"][0]["id"] == "call_9");
	REQUIRE(out["content"][0]["name"] == "lookup");
	REQUIRE(out["content"][0]["input"] == nlohmann::json({{"q", "NYC"}}));
}

TEST_CASE("response conversion tolerates malformed tool arguments", "[anthropic]") {
	nlohmann::json in = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"},
				{"tool_calls", nlohmann::json::array({
					{{"id", "call_x"}, {"function", {{"name", "f"}, {"arguments", "not json"}}}}
				})}}},
			 {"finish_reason", "tool_calls"}}
		})},
		{"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}}
	};
	auto out = convert_openai_to_anthropic_response(in, "m");
	REQUIRE(out["content"][0]["input"] == nlohmann::json::object());
}

TEST_CASE("response conversion handles empty choices", "[anthropic]") {
	auto out = convert_openai_to_anthropic_response(nlohmann::json::object(), "claude-x");
	REQUIRE(out["id"] == "msg_chatcmpl-unknown");
	REQUIRE(out["content"] == nlohmann::json::array());
	REQUIRE(out["stop_reason"] == "end_turn");
	REQUIRE(out["usage"]["input_tokens"] == 0);
	REQUIRE(out["usage"]["output_tokens"] == 0);
}

// ---------------------------------------------------------------------------
// process_line_for_anthropic_stream
// ---------------------------------------------------------------------------

TEST_CASE("stream processor ignores non-data lines", "[anthropic]") {
	AnthropicStreamState st;
	REQUIRE(process_line_for_anthropic_stream("event: ping", st).empty());
	REQUIRE(process_line_for_anthropic_stream("", st).empty());
	REQUIRE(process_line_for_anthropic_stream("   \t ", st).empty());
	REQUIRE(process_line_for_anthropic_stream("data: {not valid json", st).empty());
	REQUIRE_FALSE(st.message_started);
}

TEST_CASE("stream processor emits message_start on first content chunk", "[anthropic]") {
	AnthropicStreamState st;
	st.model = "claude-sonnet-4";
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n", st);

	REQUIRE(out.find("event: message_start") != std::string::npos);
	REQUIRE(out.find("\"type\":\"message_start\"") != std::string::npos);
	REQUIRE(out.find("claude-sonnet-4") != std::string::npos);
	REQUIRE(out.find("event: content_block_start") != std::string::npos);
	REQUIRE(out.find("\"type\":\"text\"") != std::string::npos);
	REQUIRE(out.find("event: content_block_delta") != std::string::npos);
	REQUIRE(out.find("\"text\":\"Hello\"") != std::string::npos);
	REQUIRE_FALSE(st.message_stopped);
	REQUIRE(st.message_started);
	REQUIRE(st.text_block_started);
	REQUIRE(st.output_tokens == 1);
}

TEST_CASE("stream processor emits deltas without restarting blocks", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n", st);

	REQUIRE(out.find("event: message_start") == std::string::npos);
	REQUIRE(out.find("event: content_block_start") == std::string::npos);
	REQUIRE(out.find("\"text\":\" world\"") != std::string::npos);
	REQUIRE(st.output_tokens == 2);
}

TEST_CASE("stream processor finishes on finish_reason chunk", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n", st);

	REQUIRE(out.find("event: content_block_stop") != std::string::npos);
	REQUIRE(out.find("\"index\":0") != std::string::npos);
	REQUIRE(out.find("event: message_delta") != std::string::npos);
	REQUIRE(out.find("\"stop_reason\":\"end_turn\"") != std::string::npos);
	REQUIRE(out.find("event: message_stop") != std::string::npos);
	REQUIRE(st.message_stopped);
	REQUIRE(st.finish_reason == "end_turn");
}

TEST_CASE("stream processor maps tool_calls finish to tool_use", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n", st);
	REQUIRE(out.find("\"stop_reason\":\"tool_use\"") != std::string::npos);
	REQUIRE(st.finish_reason == "tool_use");
}

TEST_CASE("stream processor handles [DONE] after content", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Bye\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream("data: [DONE]\n", st);
	REQUIRE(out.find("event: content_block_stop") != std::string::npos);
	REQUIRE(out.find("event: message_delta") != std::string::npos);
	REQUIRE(out.find("event: message_stop") != std::string::npos);
	REQUIRE(st.message_stopped);

	// Second [DONE] is a no-op
	REQUIRE(process_line_for_anthropic_stream("data: [DONE]\n", st).empty());
}

TEST_CASE("stream processor translates reasoning_content to thinking events", "[anthropic]") {
	AnthropicStreamState st;
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Hmm\"}}]}\n", st);
	REQUIRE(out.find("\"type\":\"thinking\"") != std::string::npos);
	REQUIRE(out.find("thinking_delta") != std::string::npos);
	REQUIRE(out.find("\"thinking\":\"Hmm\"") != std::string::npos);
	REQUIRE(st.thinking_started);

	// Content after thinking: signature delta, stop thinking block, text block
	out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n", st);
	REQUIRE(out.find("signature_delta") != std::string::npos);
	REQUIRE(out.find("content_block_stop") != std::string::npos);
	REQUIRE(st.thinking_finished);
	REQUIRE(st.text_block_started);
}

TEST_CASE("stream processor closes dangling thinking on [DONE]", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Hmm\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream("data: [DONE]\n", st);
	REQUIRE(out.find("signature_delta") != std::string::npos);
	REQUIRE(out.find("event: message_stop") != std::string::npos);
	REQUIRE(st.thinking_finished);
}

TEST_CASE("stream processor translates tool_calls chunks", "[anthropic]") {
	AnthropicStreamState st;
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
		"\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]}}]}\n", st);

	REQUIRE(out.find("\"type\":\"tool_use\"") != std::string::npos);
	REQUIRE(out.find("\"id\":\"call_1\"") != std::string::npos);
	REQUIRE(out.find("\"name\":\"get_weather\"") != std::string::npos);
	REQUIRE(st.tool_block_started);

	// Arguments arrive in a later chunk -> input_json_delta
	out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
		"\"function\":{\"arguments\":\"{\\\"city\\\":\\\"NYC\\\"}\"}}]}}]}\n", st);
	REQUIRE(out.find("input_json_delta") != std::string::npos);
	REQUIRE(out.find("\"partial_json\":\"{\\\"city\\\":\\\"NYC\\\"}\"") != std::string::npos);
	REQUIRE(st.output_tokens == 1);
}

TEST_CASE("stream processor emits mid-stream errors as text blocks", "[anthropic]") {
	AnthropicStreamState st;
	std::string out = process_line_for_anthropic_stream(
		"data: {\"error\":{\"message\":\"boom\"}}\n", st);
	REQUIRE(out.find("[Stream Error: boom]") != std::string::npos);
	REQUIRE(out.find("event: message_stop") != std::string::npos);
	REQUIRE(st.message_stopped);

	AnthropicStreamState st2;
	out = process_line_for_anthropic_stream("data: {\"error\":\"plain error\"}\n", st2);
	REQUIRE(out.find("[Stream Error: plain error]") != std::string::npos);
}

TEST_CASE("stream processor extracts usage from final chunk", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}],"
		"\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":9}}\n", st);
	REQUIRE(st.input_tokens == 7);
	REQUIRE(st.output_tokens == 9);
}

TEST_CASE("stream processor emits message_start for non-choice data", "[anthropic]") {
	AnthropicStreamState st;
	std::string out = process_line_for_anthropic_stream("data: {\"foo\":1}\n", st);
	REQUIRE(out.find("event: message_start") != std::string::npos);
	REQUIRE(st.message_started);
}

// ---------------------------------------------------------------------------
// apply_compatibility_layer
// ---------------------------------------------------------------------------

TEST_CASE("compatibility layer passes through empty and invalid bodies", "[anthropic]") {
	REQUIRE(apply_compatibility_layer("").empty());
	REQUIRE(apply_compatibility_layer("{oops") == "{oops");
	REQUIRE(apply_compatibility_layer("{\"model\":\"meta/llama-3.1-8b-instruct\"}")
		== "{\"model\":\"meta/llama-3.1-8b-instruct\"}");
}

TEST_CASE("compatibility layer removes reasoning_effort", "[anthropic]") {
	auto out = nlohmann::json::parse(
		apply_compatibility_layer("{\"model\":\"some/model\",\"reasoning_effort\":\"high\"}"));
	REQUIRE_FALSE(out.contains("reasoning_effort"));
	REQUIRE_FALSE(out.contains("chat_template_kwargs"));
}

TEST_CASE("compatibility layer enables thinking for GLM-5", "[anthropic]") {
	auto out = nlohmann::json::parse(
		apply_compatibility_layer("{\"model\":\"z-ai/glm-5.2\"}"));
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
	REQUIRE(out["chat_template_kwargs"]["clear_thinking"] == false);
}

TEST_CASE("compatibility layer handles DeepSeek models and drops tool_choice", "[anthropic]") {
	for (const auto& model : {"deepseek-ai/deepseek-v4-flash", "deepseek/deepseek-r1"}) {
		auto out = nlohmann::json::parse(apply_compatibility_layer(
			"{\"model\":\"" + std::string(model) + "\",\"tool_choice\":\"auto\"}"));
		REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
		REQUIRE(out["chat_template_kwargs"]["thinking"] == true);
		REQUIRE_FALSE(out.contains("tool_choice"));
	}
}

TEST_CASE("compatibility layer configures Nemotron models", "[anthropic]") {
	auto out = nlohmann::json::parse(apply_compatibility_layer(
		"{\"model\":\"nvidia/llama-3.1-nemotron-70b-instruct\"}"));
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
	REQUIRE(out["reasoning_budget"] == 4096);

	// reasoning_effort low -> low_effort instead of budget
	out = nlohmann::json::parse(apply_compatibility_layer(
		"{\"model\":\"nvidia/nemotron-3-super-120b-a12b\",\"reasoning_effort\":\"low\"}"));
	REQUIRE(out["chat_template_kwargs"]["low_effort"] == true);
	REQUIRE_FALSE(out.contains("reasoning_budget"));
	REQUIRE_FALSE(out.contains("reasoning_effort"));

	// existing reasoning_budget is preserved
	out = nlohmann::json::parse(apply_compatibility_layer(
		"{\"model\":\"nvidia/nemotron-3-ultra-550b-a55b\",\"reasoning_budget\":1000}"));
	REQUIRE(out["reasoning_budget"] == 1000);
}

TEST_CASE("compatibility layer enables thinking for Qwen and DiffusionGemma", "[anthropic]") {
	auto out = nlohmann::json::parse(
		apply_compatibility_layer("{\"model\":\"qwen/qwen-2.5-72b-instruct\"}"));
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);

	out = nlohmann::json::parse(
		apply_compatibility_layer("{\"model\":\"google/diffusiongemma-26b-a4b-it\"}"));
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
}

TEST_CASE("compatibility layer merges into existing chat_template_kwargs", "[anthropic]") {
	auto out = nlohmann::json::parse(apply_compatibility_layer(
		"{\"model\":\"qwen/qwen-2.5\",\"chat_template_kwargs\":{\"custom\":1}}"));
	REQUIRE(out["chat_template_kwargs"]["custom"] == 1);
	REQUIRE(out["chat_template_kwargs"]["enable_thinking"] == true);
}

TEST_CASE("compatibility layer uses original_model when provided", "[anthropic]") {
	auto out = nlohmann::json::parse(apply_compatibility_layer(
		"{\"model\":\"some/other-model\",\"reasoning_effort\":\"low\"}",
		"nvidia/nemotron-3-super-120b-a12b"));
	REQUIRE(out["chat_template_kwargs"]["low_effort"] == true);
}

TEST_CASE("stream processor accepts the reasoning field for thinking", "[anthropic]") {
	AnthropicStreamState st;
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"reasoning\":\"thinking text\"}}]}\n", st);
	REQUIRE(out.find("\"type\":\"thinking\"") != std::string::npos);
	REQUIRE(out.find("thinking_delta") != std::string::npos);
	REQUIRE(out.find("\"thinking\":\"thinking text\"") != std::string::npos);
	REQUIRE(st.thinking_started);
	REQUIRE(st.thinking_block_started);
}

TEST_CASE("stream processor closes thinking and text blocks when tool_calls arrive", "[anthropic]") {
	// thinking -> tool_calls transition emits a signature delta + stop
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Hmm\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
		"\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]}}]}\n", st);
	REQUIRE(out.find("signature_delta") != std::string::npos);
	REQUIRE(out.find("\"type\":\"tool_use\"") != std::string::npos);
	REQUIRE(st.thinking_finished);
	REQUIRE(st.tool_block_started);

	// text -> tool_calls transition stops the text block first
	AnthropicStreamState st2;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Let me look\"}}]}\n", st2);
	out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_2\","
		"\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]}}]}\n", st2);
	REQUIRE(out.find("content_block_stop") != std::string::npos);
	REQUIRE(out.find("\"id\":\"call_2\"") != std::string::npos);
	REQUIRE_FALSE(st2.text_block_started);
	REQUIRE(st2.tool_block_started);
}

TEST_CASE("stream processor closes tool block when text resumes", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
		"\"function\":{\"name\":\"lookup\",\"arguments\":\"\"}}]}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"content\":\"Done\"}}]}\n", st);
	REQUIRE(out.find("content_block_stop") != std::string::npos);
	REQUIRE(out.find("\"type\":\"text\"") != std::string::npos);
	REQUIRE_FALSE(st.tool_block_started);
	REQUIRE(st.text_block_started);
}

TEST_CASE("stream processor finishes dangling thinking on finish_reason", "[anthropic]") {
	AnthropicStreamState st;
	process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Hmm\"}}]}\n", st);
	std::string out = process_line_for_anthropic_stream(
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n", st);
	REQUIRE(out.find("signature_delta") != std::string::npos);
	REQUIRE(out.find("content_block_stop") != std::string::npos);
	REQUIRE(out.find("\"stop_reason\":\"max_tokens\"") != std::string::npos);
	REQUIRE(st.thinking_finished);
	REQUIRE(st.message_stopped);
}

TEST_CASE("request conversion handles string tool_result content", "[anthropic]") {
	auto out = convert_anthropic_to_openai_request({
		{"messages", nlohmann::json::array({
			{{"role", "user"}, {"content", nlohmann::json::array({
				{{"type", "tool_result"}, {"tool_use_id", "toolu_1"},
				 {"content", "plain string result"}}
			})}}
		})}
	});
	REQUIRE(out["messages"].size() == 1);
	REQUIRE(out["messages"][0]["role"] == "tool");
	REQUIRE(out["messages"][0]["tool_call_id"] == "toolu_1");
	REQUIRE(out["messages"][0]["content"] == "plain string result");
}

TEST_CASE("response conversion accepts reasoning field", "[anthropic]") {
	nlohmann::json in = {
		{"choices", nlohmann::json::array({
			{{"message", {{"role", "assistant"},
				{"reasoning", "quiet thoughts"},
				{"content", "Answer"}}},
			 {"finish_reason", "stop"}}
		})}
	};
	auto out = convert_openai_to_anthropic_response(in, "m");
	REQUIRE(out["content"][0]["type"] == "thinking");
	REQUIRE(out["content"][0]["thinking"] == "quiet thoughts");
}

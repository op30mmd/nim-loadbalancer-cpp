#include "anthropic_handler.h"
#include <algorithm>
#include <cstring>
#include "utils.h"

// ============================================================================
// Anthropic Compatibility Helpers
// ============================================================================

std::string gen_request_id() {
	return gen_random_id("req_", 100000000);
}

nlohmann::json make_anthropic_error(const std::string& type, const std::string& message) {
	return {
		{"type", "error"},
		{"error", {{"type", type}, {"message", message}}}
	};
}

// ============================================================================
// Stream Processor
// ============================================================================

std::string process_line_for_anthropic_stream(const std::string& line, AnthropicStreamState& state) {
	std::string line_stripped = line;
	line_stripped.erase(0, line_stripped.find_first_not_of(" \t\n\r"));
	line_stripped.erase(line_stripped.find_last_not_of(" \t\n\r") + 1);

	if (line_stripped.rfind("data: ", 0) != 0) {
		return "";
	}

	std::string json_str = line_stripped.substr(6);
	json_str.erase(0, json_str.find_first_not_of(" \t\n\r"));

	if (json_str == "[DONE]") {
		std::string out = "";
		if (state.thinking_started && !state.thinking_finished) {
			if (state.thinking_block_started) {
				nlohmann::json sig_delta = {
					{"type", "content_block_delta"},
					{"index", state.current_block_index},
					{"delta", {{"type", "signature_delta"}, {"signature", ""}}}
				};
				out += "event: content_block_delta\ndata: " + sig_delta.dump() + "\n\n";
			}
			state.thinking_finished = true;
		}

		if (state.text_block_started || state.thinking_block_started || state.tool_block_started) {
			out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
			state.text_block_started = false;
			state.thinking_block_started = false;
			state.tool_block_started = false;
		}

		if (!state.message_stopped) {
			out += "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" + state.finish_reason + "\",\"stop_sequence\":null},\"usage\":{\"input_tokens\":" + std::to_string(state.input_tokens) + ",\"output_tokens\":" + std::to_string(state.output_tokens) + "}}\n\n";
			out += "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
			state.message_stopped = true;
		}
		return out;
	}

	try {
		nlohmann::json parsed = nlohmann::json::parse(json_str);
		std::string out = "";

		// Check for NIM error in SSE data mid-stream
		if (parsed.contains("error")) {
			std::string err_msg = "Upstream error";
			if (parsed["error"].is_string()) {
				err_msg = parsed["error"].get<std::string>();
			} else if (parsed["error"].is_object() && parsed["error"].contains("message")) {
				err_msg = parsed["error"]["message"].get<std::string>();
			}

			if (state.thinking_block_started || state.text_block_started || state.tool_block_started) {
				out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":"
					+ std::to_string(state.current_block_index) + "}\n\n";
				state.thinking_block_started = false;
				state.text_block_started = false;
				state.tool_block_started = false;
			}

			state.current_block_index++;
			out += "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":"
				+ std::to_string(state.current_block_index)
				+ ",\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
			out += "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":"
				+ std::to_string(state.current_block_index)
				+ ",\"delta\":{\"type\":\"text_delta\",\"text\":\"[Stream Error: " + err_msg + "]\"}}\n\n";
			out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":"
				+ std::to_string(state.current_block_index) + "}\n\n";

			if (!state.message_stopped) {
				out += "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"input_tokens\":" + std::to_string(state.input_tokens) + ",\"output_tokens\":" + std::to_string(state.output_tokens) + "}}\n\n";
				out += "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
				state.message_stopped = true;
			}

			LOG_WARN("AnthropicStream", "NIM sent error mid-stream: " + err_msg);
			return out;
		}

		if (!state.message_started) {
			// Preserve original Anthropic model name (set from fallback_model).
			// Do not overwrite from NIM response — clients expect the name they sent.
			nlohmann::json msg_start = {
				{"type", "message_start"},
				{"message", {
					{"id", state.msg_id},
					{"type", "message"},
					{"role", "assistant"},
					{"model", state.model},
					{"content", nlohmann::json::array()},
					{"stop_reason", nullptr},
					{"stop_sequence", nullptr},
					{"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
				}}
			};
			out += "event: message_start\ndata: " + msg_start.dump() + "\n\n";
			state.message_started = true;
		}

		if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
			const auto& choice = parsed["choices"][0];
			if (choice.contains("delta") && choice["delta"].is_object()) {
				const auto& delta = choice["delta"];

				std::string reasoning = "";
				if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
					reasoning = delta["reasoning_content"].get<std::string>();
				}
				else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
					reasoning = delta["reasoning"].get<std::string>();
				}

				std::string content = "";
				if (delta.contains("content") && delta["content"].is_string()) {
					content = delta["content"].get<std::string>();
				}

				nlohmann::json tool_calls_delta = nlohmann::json::array();
				if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
					tool_calls_delta = delta["tool_calls"];
				}

				if (!tool_calls_delta.empty()) {
					for (const auto& tc : tool_calls_delta) {
						if (!tc.is_object()) continue;

						if (state.thinking_started && !state.thinking_finished) {
							if (state.thinking_block_started) {
								nlohmann::json sig_delta = {
									{"type", "content_block_delta"},
									{"index", state.current_block_index},
									{"delta", {{"type", "signature_delta"}, {"signature", ""}}}
								};
								out += "event: content_block_delta\ndata: " + sig_delta.dump() + "\n\n";
							}
							state.thinking_finished = true;
						}

						if (state.text_block_started) {
							out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
							state.text_block_started = false;
						}

						std::string tc_id = tc.value("id", "");
						std::string tc_name = "";
						std::string args_chunk = "";

						if (tc.contains("function") && tc["function"].is_object()) {
							tc_name = tc["function"].value("name", "");
							if (tc["function"].contains("arguments") && tc["function"]["arguments"].is_string()) {
								args_chunk = tc["function"]["arguments"].get<std::string>();
							}
						}

						if (!tc_id.empty() || !tc_name.empty()) {
							if (state.tool_block_started) {
								out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
								state.tool_block_started = false;
							}

							if (!tc_id.empty()) state.current_tool_id = tc_id;
							if (!tc_name.empty()) state.current_tool_name = tc_name;

							state.current_block_index++;
							nlohmann::json blk_start = {
								{"type", "content_block_start"},
								{"index", state.current_block_index},
								{"content_block", {
									{"type", "tool_use"},
									{"id", state.current_tool_id},
									{"name", state.current_tool_name},
									{"input", nlohmann::json::object()}
								}}
							};
							out += "event: content_block_start\ndata: " + blk_start.dump() + "\n\n";
							state.tool_block_started = true;
						}

						if (!args_chunk.empty()) {
							if (!state.tool_block_started) {
								state.current_block_index++;
								nlohmann::json blk_start = {
									{"type", "content_block_start"},
									{"index", state.current_block_index},
									{"content_block", {
										{"type", "tool_use"},
										{"id", gen_random_id("call_", 100000)},
										{"name", "unknown_tool"},
										{"input", nlohmann::json::object()}
									}}
								};
								out += "event: content_block_start\ndata: " + blk_start.dump() + "\n\n";
								state.tool_block_started = true;
							}

							nlohmann::json blk_delta = {
								{"type", "content_block_delta"},
								{"index", state.current_block_index},
								{"delta", {
									{"type", "input_json_delta"},
									{"partial_json", args_chunk}
								}}
							};
							out += "event: content_block_delta\ndata: " + blk_delta.dump() + "\n\n";
							state.output_tokens++;
						}
					}
				}
				else {
					std::string text_to_emit = "";

					if (!reasoning.empty()) {
						if (!state.thinking_started) {
							if (state.tool_block_started) {
								out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
								state.tool_block_started = false;
							}
							if (state.text_block_started) {
								out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
								state.text_block_started = false;
							}
							state.current_block_index++;
							nlohmann::json think_start = {
								{"type", "content_block_start"},
								{"index", state.current_block_index},
								{"content_block", {{"type", "thinking"}, {"thinking", ""}}}
							};
							out += "event: content_block_start\ndata: " + think_start.dump() + "\n\n";
							state.thinking_started = true;
							state.thinking_block_started = true;

							nlohmann::json think_delta = {
								{"type", "content_block_delta"},
								{"index", state.current_block_index},
								{"delta", {{"type", "thinking_delta"}, {"thinking", reasoning}}}
							};
							out += "event: content_block_delta\ndata: " + think_delta.dump() + "\n\n";
							state.output_tokens++;
						}
						else if (state.thinking_block_started) {
							nlohmann::json think_delta = {
								{"type", "content_block_delta"},
								{"index", state.current_block_index},
								{"delta", {{"type", "thinking_delta"}, {"thinking", reasoning}}}
							};
							out += "event: content_block_delta\ndata: " + think_delta.dump() + "\n\n";
							state.output_tokens++;
						}
					}
					else if (!content.empty()) {
						if (state.thinking_started && !state.thinking_finished) {
							nlohmann::json sig_delta = {
								{"type", "content_block_delta"},
								{"index", state.current_block_index},
								{"delta", {{"type", "signature_delta"}, {"signature", ""}}}
							};
							out += "event: content_block_delta\ndata: " + sig_delta.dump() + "\n\n";
							out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
							state.thinking_block_started = false;
							state.thinking_finished = true;
						}

						if (state.tool_block_started) {
							out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
							state.tool_block_started = false;
						}

						if (!state.text_block_started) {
							state.current_block_index++;
							nlohmann::json blk_start = {
								{"type", "content_block_start"},
								{"index", state.current_block_index},
								{"content_block", {{"type", "text"}, {"text", ""}}}
							};
							out += "event: content_block_start\ndata: " + blk_start.dump() + "\n\n";
							state.text_block_started = true;
						}

						nlohmann::json blk_delta = {
							{"type", "content_block_delta"},
							{"index", state.current_block_index},
							{"delta", {{"type", "text_delta"}, {"text", content}}}
						};
						out += "event: content_block_delta\ndata: " + blk_delta.dump() + "\n\n";
						state.output_tokens++;
					}
				}
			}

			// Extract usage info from NIM SSE final chunk
			if (parsed.contains("usage") && parsed["usage"].is_object()) {
				if (parsed["usage"].contains("completion_tokens") && parsed["usage"]["completion_tokens"].is_number_integer()) {
					state.output_tokens = parsed["usage"]["completion_tokens"].get<int>();
				}
				if (parsed["usage"].contains("prompt_tokens") && parsed["usage"]["prompt_tokens"].is_number_integer()) {
					state.input_tokens = parsed["usage"]["prompt_tokens"].get<int>();
				}
			}

			if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
				std::string fr = choice["finish_reason"].get<std::string>();
				std::string stop_reason = (fr == "tool_calls") ? "tool_use" : ((fr == "length") ? "max_tokens" : "end_turn");
				state.finish_reason = stop_reason;

				if (state.thinking_started && !state.thinking_finished) {
					if (state.thinking_block_started) {
						nlohmann::json sig_delta = {
							{"type", "content_block_delta"},
							{"index", state.current_block_index},
							{"delta", {{"type", "signature_delta"}, {"signature", ""}}}
						};
						out += "event: content_block_delta\ndata: " + sig_delta.dump() + "\n\n";
					}
					state.thinking_finished = true;
				}

				if (state.text_block_started || state.thinking_block_started || state.tool_block_started) {
					out += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":" + std::to_string(state.current_block_index) + "}\n\n";
					state.text_block_started = false;
					state.thinking_block_started = false;
					state.tool_block_started = false;
				}

				if (!state.message_stopped) {
					nlohmann::json msg_delta = {
						{"type", "message_delta"},
						{"delta", {{"stop_reason", stop_reason}, {"stop_sequence", nullptr}}},
						{"usage", {{"input_tokens", state.input_tokens}, {"output_tokens", state.output_tokens}}}
					};
					out += "event: message_delta\ndata: " + msg_delta.dump() + "\n\n";
					out += "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
					state.message_stopped = true;
				}
			}
		}
		return out;
	}
	catch (...) {
		return "";
	}
}

// ============================================================================
// Request Conversion: Anthropic -> OpenAI
// ============================================================================

nlohmann::json convert_anthropic_to_openai_request(const nlohmann::json& anthropic_json) {
	nlohmann::json openai_json = nlohmann::json::object();

	if (anthropic_json.contains("model") && anthropic_json["model"].is_string()) {
		openai_json["model"] = map_anthropic_model_to_nim(anthropic_json["model"].get<std::string>());
	}

	if (anthropic_json.contains("stream") && anthropic_json["stream"].is_boolean()) {
		openai_json["stream"] = anthropic_json["stream"].get<bool>();
	}
	else {
		openai_json["stream"] = false;
	}

	if (anthropic_json.contains("max_tokens") && anthropic_json["max_tokens"].is_number_integer()) {
		openai_json["max_tokens"] = anthropic_json["max_tokens"].get<int>();
	}

	if (anthropic_json.contains("temperature")) {
		double temp = anthropic_json["temperature"].is_number() ? anthropic_json["temperature"].get<double>() : 1.0;
		openai_json["temperature"] = (temp < 0.0) ? 0.0 : ((temp > 1.0) ? 1.0 : temp);
	}
	if (anthropic_json.contains("top_p")) openai_json["top_p"] = anthropic_json["top_p"];
	if (anthropic_json.contains("top_k")) openai_json["top_k"] = anthropic_json["top_k"];
	if (anthropic_json.contains("stop_sequences") && anthropic_json["stop_sequences"].is_array()) {
		openai_json["stop"] = anthropic_json["stop_sequences"];
	}
	if (anthropic_json.contains("metadata") && anthropic_json["metadata"].is_object()) {
		if (anthropic_json["metadata"].contains("user_id") && anthropic_json["metadata"]["user_id"].is_string()) {
			openai_json["user"] = anthropic_json["metadata"]["user_id"].get<std::string>();
		}
	}

	std::string system_str = "";
	if (anthropic_json.contains("system")) {
		if (anthropic_json["system"].is_string()) {
			system_str = anthropic_json["system"].get<std::string>();
		}
		else if (anthropic_json["system"].is_array()) {
			for (const auto& item : anthropic_json["system"]) {
				if (item.is_object() && item.contains("text") && item["text"].is_string()) {
					system_str += item["text"].get<std::string>() + "\n";
				}
			}
		}
	}

	nlohmann::json openai_messages = nlohmann::json::array();
	if (!system_str.empty()) {
		openai_messages.push_back({ {"role", "system"}, {"content", system_str} });
	}

	if (anthropic_json.contains("messages") && anthropic_json["messages"].is_array()) {
		for (const auto& msg : anthropic_json["messages"]) {
			if (!msg.is_object()) continue;
			std::string role = msg.value("role", "user");

			if (msg.contains("content")) {
				if (msg["content"].is_string()) {
					openai_messages.push_back({ {"role", role}, {"content", msg["content"].get<std::string>()} });
				}
				else if (msg["content"].is_array()) {
					std::string aggregated_text = "";
					nlohmann::json tool_calls = nlohmann::json::array();

					for (const auto& block : msg["content"]) {
						if (!block.is_object()) continue;
						std::string type = block.value("type", "");

						if (type == "text" && block.contains("text") && block["text"].is_string()) {
							aggregated_text += block["text"].get<std::string>();
						}
						else if (type == "tool_use") {
							std::string tool_id = block.value("id", "");
							std::string tool_name = block.value("name", "");
							nlohmann::json tool_input = block.contains("input") ? block["input"] : nlohmann::json::object();
							tool_calls.push_back({
								{"id", tool_id},
								{"type", "function"},
								{"function", {{"name", tool_name}, {"arguments", tool_input.dump()}}}
								});
						}
						else if (type == "tool_result") {
							std::string tool_use_id = block.value("tool_use_id", "");
							std::string result_content = "";
							if (block.contains("content")) {
								if (block["content"].is_string()) {
									result_content = block["content"].get<std::string>();
								}
								else if (block["content"].is_array()) {
									for (const auto& sub : block["content"]) {
										if (sub.is_object() && sub.value("type", "") == "text" && sub.contains("text")) {
											result_content += sub["text"].get<std::string>();
										}
									}
								}
							}
							openai_messages.push_back({
								{"role", "tool"},
								{"tool_call_id", tool_use_id},
								{"content", result_content}
								});
						}
					}

					if (!aggregated_text.empty() || !tool_calls.empty()) {
						nlohmann::json assistant_msg = { {"role", role} };
						if (!aggregated_text.empty()) assistant_msg["content"] = aggregated_text;
						if (!tool_calls.empty()) assistant_msg["tool_calls"] = tool_calls;
						openai_messages.push_back(assistant_msg);
					}
				}
			}
		}
	}
	openai_json["messages"] = openai_messages;

	if (anthropic_json.contains("tools") && anthropic_json["tools"].is_array()) {
		nlohmann::json openai_tools = nlohmann::json::array();
		for (const auto& tool : anthropic_json["tools"]) {
			if (!tool.is_object()) continue;
			nlohmann::json fn = {
				{"name", tool.value("name", "")},
				{"description", tool.value("description", "")},
				{"parameters", tool.contains("input_schema") ? tool["input_schema"] : nlohmann::json::object()}
			};
			openai_tools.push_back({ {"type", "function"}, {"function", fn} });
		}
		if (!openai_tools.empty()) {
			openai_json["tools"] = openai_tools;
		}
	}

	if (anthropic_json.contains("tool_choice") && anthropic_json["tool_choice"].is_object()) {
		const auto& tc = anthropic_json["tool_choice"];
		std::string tc_type = tc.value("type", "auto");
		if (tc_type == "auto") {
			openai_json["tool_choice"] = "auto";
		}
		else if (tc_type == "any") {
			openai_json["tool_choice"] = "required";
		}
		else if (tc_type == "tool") {
			std::string tool_name = tc.value("name", "");
			openai_json["tool_choice"] = {
				{"type", "function"},
				{"function", {{"name", tool_name}}}
			};
		}
		else if (tc_type == "none") {
			openai_json.erase("tools");
			openai_json.erase("tool_choice");
		}
	}

	if (anthropic_json.contains("thinking") && anthropic_json["thinking"].is_object()) {
		auto& thinking_obj = anthropic_json["thinking"];
		if (thinking_obj.value("type", "") == "enabled") {
			openai_json["chat_template_kwargs"] = { {"enable_thinking", true} };
			if (thinking_obj.contains("budget_tokens") && thinking_obj["budget_tokens"].is_number_integer()) {
				openai_json["reasoning_budget"] = thinking_obj["budget_tokens"].get<int>();
			}
		}
	}

	return openai_json;
}

// ============================================================================
// Response Conversion: OpenAI -> Anthropic
// ============================================================================

nlohmann::json convert_openai_to_anthropic_response(const nlohmann::json& openai_json, const std::string& fallback_model) {
	nlohmann::json anthropic_res = nlohmann::json::object();

	std::string msg_id = "msg_" + openai_json.value("id", "chatcmpl-unknown");
	std::string model = fallback_model;

	anthropic_res["id"] = msg_id;
	anthropic_res["type"] = "message";
	anthropic_res["role"] = "assistant";
	anthropic_res["model"] = model;

	nlohmann::json content_blocks = nlohmann::json::array();
	std::string finish_reason = "end_turn";

	if (openai_json.contains("choices") && openai_json["choices"].is_array() && !openai_json["choices"].empty()) {
		const auto& choice = openai_json["choices"][0];
		if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
			std::string fr = choice["finish_reason"].get<std::string>();
			if (fr == "tool_calls") finish_reason = "tool_use";
			else if (fr == "length") finish_reason = "max_tokens";
			else finish_reason = "end_turn";
		}

		if (choice.contains("message") && choice["message"].is_object()) {
			const auto& message = choice["message"];

			std::string reasoning = "";
			if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
				reasoning = message["reasoning_content"].get<std::string>();
			}
			else if (message.contains("reasoning") && message["reasoning"].is_string()) {
				reasoning = message["reasoning"].get<std::string>();
			}

			std::string text = "";
			if (message.contains("content") && message["content"].is_string()) {
				text = message["content"].get<std::string>();
			}

			if (!reasoning.empty()) {
				content_blocks.push_back({
					{"type", "thinking"},
					{"thinking", reasoning},
					{"signature", ""}
				});
			}

			if (!text.empty()) {
				content_blocks.push_back({
					{"type", "text"},
					{"text", text}
				});
			}

			if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
				for (const auto& tc : message["tool_calls"]) {
					if (!tc.is_object()) continue;
					std::string tool_id = tc.value("id", "");
					std::string tool_name = "";
					nlohmann::json tool_input = nlohmann::json::object();

					if (tc.contains("function") && tc["function"].is_object()) {
						tool_name = tc["function"].value("name", "");
						std::string args_str = tc["function"].value("arguments", "{}");
						try {
							tool_input = nlohmann::json::parse(args_str);
						}
						catch (...) {
							tool_input = nlohmann::json::object();
						}
					}

					content_blocks.push_back({
						{"type", "tool_use"},
						{"id", tool_id},
						{"name", tool_name},
						{"input", tool_input}
					});
				}
			}
		}
	}

	anthropic_res["content"] = content_blocks;
	anthropic_res["stop_reason"] = finish_reason;
	anthropic_res["stop_sequence"] = nullptr;

	int prompt_tokens = 0;
	int completion_tokens = 0;
	if (openai_json.contains("usage") && openai_json["usage"].is_object()) {
		prompt_tokens = openai_json["usage"].value("prompt_tokens", 0);
		completion_tokens = openai_json["usage"].value("completion_tokens", 0);
	}

	anthropic_res["usage"] = {
		{"input_tokens", prompt_tokens},
		{"output_tokens", completion_tokens}
	};

	return anthropic_res;
}

// ============================================================================
// Compatibility Layer: Model-Specific Overrides
// ============================================================================

std::string apply_compatibility_layer(const std::string& body_str, const std::string& original_model) {
	if (body_str.empty()) return body_str;
	try {
		nlohmann::json body_json = nlohmann::json::parse(body_str);
		// Use original_model (the model the client actually requested) if provided,
		// otherwise fall back to the model in the request body.
		std::string model_req = original_model.empty() ?
			(body_json.contains("model") && body_json["model"].is_string() ? body_json["model"].get<std::string>() : "unknown")
			: original_model;
		std::string model_lower = model_req;
		std::transform(model_lower.begin(), model_lower.end(), model_lower.begin(), [](unsigned char c) { return std::tolower(c); });

		bool modified = false;
		std::string reasoning_effort = "";
		if (body_json.contains("reasoning_effort")) {
			if (body_json["reasoning_effort"].is_string()) {
				reasoning_effort = body_json["reasoning_effort"].get<std::string>();
			}
			body_json.erase("reasoning_effort");
			modified = true;
		}

		if (!body_json.contains("chat_template_kwargs") || !body_json["chat_template_kwargs"].is_object()) {
			body_json["chat_template_kwargs"] = nlohmann::json::object();
		}

		if (model_lower.find("glm-5") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			body_json["chat_template_kwargs"]["clear_thinking"] = false;
			modified = true;
		}
		else if (model_lower.find("deepseek-v4") != std::string::npos || model_lower.find("deepseek-r1") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			body_json["chat_template_kwargs"]["thinking"] = true;
			if (body_json.contains("tool_choice")) {
				body_json.erase("tool_choice");
			}
			modified = true;
		}
		else if (model_lower.find("nemotron") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			if (reasoning_effort == "low") {
				body_json["chat_template_kwargs"]["low_effort"] = true;
			}
			else {
				if (!body_json.contains("reasoning_budget")) {
					body_json["reasoning_budget"] = 4096;
				}
			}
			modified = true;
		}
		else if (model_lower.find("qwen") != std::string::npos || model_lower.find("diffusiongemma") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			modified = true;
		}

		if (body_json["chat_template_kwargs"].empty()) {
			body_json.erase("chat_template_kwargs");
		}

		if (modified) {
			return body_json.dump();
		}
	}
	catch (...) {}
	return body_str;
}

#define _CRT_SECURE_NO_WARNINGS

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 26495)
#pragma warning(disable: 6386)
#pragma warning(disable: 6262)
#endif

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "logger.h"
#include "proxy_config.h"
#include "key_manager.h"
#include "utils.h"
#include "anthropic_handler.h"

int main() {
	g_logger.init(LogLevel::LEVEL_DEBUG, "proxy.log");
	curl_global_init(CURL_GLOBAL_ALL);
	std::srand(static_cast<unsigned>(std::time(nullptr)));

	LOG_INFO("Startup", "Initializing NVIDIA NIM Load Balancer...");

	auto keys = load_api_keys();
	LOG_INFO("Startup", "Loaded " + std::to_string(keys.size()) + " API key(s).");

	auto env_int = [](const char* name, int fallback) -> int {
		const char* raw = std::getenv(name);
		if (raw && *raw) {
			try { return std::stoi(raw); } catch (...) {}
		}
		return fallback;
	};
	int key_cooldown = env_int("KEY_COOLDOWN_SECONDS", 60);
	int key_max_cooldown = env_int("KEY_MAX_COOLDOWN_SECONDS", 1800);

	KeyManager key_manager(keys, key_cooldown, key_max_cooldown);
	ClientSideBackoff backoff_manager(2, 1.0);
	ModelCache model_cache(3600);

	LOG_INFO("Startup", "Key rotation: round-robin with adaptive backoff | keys=" + std::to_string(keys.size())
		+ " | base=" + std::to_string(key_cooldown) + "s"
		+ " | cap=" + std::to_string(key_max_cooldown) + "s");

	std::atomic<bool> shutdown(false);
	std::thread sync_thread(run_sync_config_task, std::ref(key_manager), std::ref(shutdown));

	httplib::Server svr;

	svr.set_read_timeout(300, 0);
	svr.set_write_timeout(600, 0);

	// --- GLOBAL CORS MIDDLEWARE & OPTIONS PREFLIGHT HANDLER ---
	svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
		res.set_header("Access-Control-Allow-Origin", "*");
		res.set_header("Access-Control-Allow-Headers", "*");
		res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH");
		res.set_header("x-request-id", gen_request_id());
		if (req.method == "OPTIONS") {
			res.status = 204;
			return httplib::Server::HandlerResponse::Handled;
		}
		return httplib::Server::HandlerResponse::Unhandled;
	});

	// --- ENDPOINT: GET /health ---
	svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
		nlohmann::json h = {{"status", "ok"}, {"service", "nim-proxy-cpp"}};
		res.status = 200;
		res.set_content(h.dump(), "application/json");
	});

	// --- ENDPOINT: GET /v1/keys (rotation health, masked) ---
	svr.Get("/v1/keys", [&](const httplib::Request&, httplib::Response& res) {
		auto snap = key_manager.snapshot();
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& s : snap) {
			arr.push_back({
				{"key", s.masked},
				{"state", s.state},
				{"consecutive_failures", s.consecutive_failures},
				{"total_requests", s.total_requests},
				{"total_successes", s.total_successes},
				{"total_failures", s.total_failures},
				{"cooldown_seconds_remaining", s.cooldown_remaining_sec}
			});
		}
		res.status = 200;
		res.set_content(nlohmann::json({{"strategy", "round-robin"}, {"keys", arr}}).dump(), "application/json");
	});

	// --- ENDPOINT: GET /v1/models & GET /models ---
	auto models_handler = [&](const httplib::Request& req, httplib::Response& res) {
		LOG_INFO("ModelsAPI", "Catalog request received from " + req.remote_addr);
		std::string cached = model_cache.get();
		if (!cached.empty()) {
			LOG_DEBUG("ModelsAPI", "Serving model catalog from cache.");
			res.status = 200;
			res.set_content(cached, "application/json");
			return;
		}

		size_t max_key_retries = key_manager.get_keys_count();
		if (max_key_retries == 0) {
			LOG_ERROR("ModelsAPI", "No API keys configured.");
			res.status = 503;
			res.set_content("{\"error\": \"No API keys configured\"}", "application/json");
			return;
		}

		for (size_t i = 0; i < max_key_retries; ++i) {
			std::string key = key_manager.get_key();
			if (key.empty()) {
				res.status = 503;
				res.set_content("{\"error\": \"All API keys in cooldown\"}", "application/json");
				return;
			}

			CURL* curl = curl_easy_init();
			if (!curl) {
				key_manager.mark_failed(key, 500);
				continue;
			}

			std::string url = NVIDIA_BASE_URL + "/models";
			struct curl_slist* headers_list = nullptr;
			std::string auth_header = "Authorization: Bearer " + key;
			headers_list = curl_slist_append(headers_list, auth_header.c_str());
			headers_list = curl_slist_append(headers_list, "User-Agent: nim-proxy-cpp/1.0");
			headers_list = curl_slist_append(headers_list, "X-Source: nim-proxy-cpp");

			CurlBuffer write_buf;
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
			configure_curl_network_stability(curl);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_buffer_callback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_buf);

			CURLcode curl_res = curl_easy_perform(curl);
			long http_code = 0;
			if (curl_res == CURLE_OK) {
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
			}

			curl_slist_free_all(headers_list);
			curl_easy_cleanup(curl);

			if (curl_res != CURLE_OK) {
				LOG_WARN("ModelsAPI", "Transient network error: " + std::string(curl_easy_strerror(curl_res)) + " (Retrying...)");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 1000)));
				continue;
			}

			if (http_code == 429 || http_code == 401 || http_code == 403) {
				key_manager.mark_failed(key, http_code);
				continue;
			}

		if (http_code == 200) {
			key_manager.mark_success(key);
			LOG_INFO("ModelsAPI", "Catalog successfully fetched and cached.");
				model_cache.set(write_buf.data);
				res.status = 200;
				res.set_content(write_buf.data, "application/json");
				return;
			}

			res.status = http_code;
			res.set_content("{\"error\": \"Failed fetching models\"}", "application/json");
			return;
		}

		res.status = 502;
		res.set_content("{\"error\": \"Failed to fetch models from NVIDIA\"}", "application/json");
	};

	svr.Get("/v1/models", models_handler);
	svr.Get("/models", models_handler);

	// --- ANTHROPIC MESSAGES ENDPOINTS: POST /v1/messages & POST /messages ---
	auto handle_anthropic_messages = [&](const httplib::Request& req, httplib::Response& res) {
		res.set_header("anthropic-version", "2023-06-01");

		auto anthropic_json = std::make_unique<nlohmann::json>();
		try {
			*anthropic_json = nlohmann::json::parse(req.body);
		}
		catch (...) {
			LOG_WARN("AnthropicAPI", "Invalid JSON request payload from " + req.remote_addr);
			res.status = 400;
			res.set_content(make_anthropic_error("invalid_request_error", "Invalid JSON").dump(), "application/json");
			return;
		}

		if (!anthropic_json->contains("max_tokens")) {
			res.status = 400;
			res.set_content(make_anthropic_error("invalid_request_error", "max_tokens: required").dump(), "application/json");
			return;
		}

		int max_tokens_val = anthropic_json->value("max_tokens", 0);
		if (max_tokens_val == 0) {
			std::string empty_model = anthropic_json->value("model", "unknown");
			nlohmann::json empty_res = {
				{"id", gen_request_id()},
				{"type", "message"},
				{"role", "assistant"},
				{"model", empty_model},
				{"content", nlohmann::json::array()},
				{"stop_reason", "end_turn"},
				{"stop_sequence", nullptr},
				{"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
			};
			res.status = 200;
			res.set_content(empty_res.dump(), "application/json");
			LOG_INFO("AnthropicAPI", "Cache pre-warming request (max_tokens=0), returning empty response.");
			return;
		}

		std::string fallback_model = anthropic_json->value("model", "unknown");
		bool is_stream = anthropic_json->value("stream", false);
		LOG_INFO("AnthropicAPI", "Request from " + req.remote_addr + " | Model: " + fallback_model + " | Stream: " + (is_stream ? "true" : "false"));

		nlohmann::json openai_req = convert_anthropic_to_openai_request(*anthropic_json);

		// When thinking is enabled, Anthropic uses separate pools: max_tokens for
		// output and budget_tokens for thinking. NIM uses a single max_tokens for
		// total output (thinking + content). Scale up max_tokens so the model has
		// room for both thinking and actual output.
		if (openai_req.contains("max_tokens") && openai_req["max_tokens"].is_number_integer()) {
			int requested_max = openai_req["max_tokens"].get<int>();

			if (anthropic_json->contains("thinking") && (*anthropic_json)["thinking"].is_object()
				&& (*anthropic_json)["thinking"].value("type", "") == "enabled") {
				int budget = (*anthropic_json)["thinking"].value("budget_tokens", 0);
				int output_tokens = openai_req["max_tokens"].get<int>();
				if (budget > 0) {
					// Cap the total: NIM models may not handle very large max_tokens
					requested_max = std::min(output_tokens + budget, 8192);
					LOG_INFO("AnthropicAPI", "Thinking enabled: scaled max_tokens from "
						+ std::to_string(output_tokens)
						+ " to " + std::to_string(requested_max)
						+ " (budget_tokens=" + std::to_string(budget) + ")");
				}
				else {
					// No explicit budget — use a reasonable default
					requested_max = std::min(std::max(output_tokens * 4, 4096), 8192);
					LOG_INFO("AnthropicAPI", "Thinking enabled: scaled max_tokens to "
						+ std::to_string(requested_max) + " (no budget_tokens set)");
				}
			}

			int estimated_input = estimate_input_tokens(*anthropic_json);
			std::string model_id = openai_req.value("model", fallback_model);
			int context_window = get_model_context_window(model_id);

			int available = context_window - estimated_input;
			int upper = std::max(1, available);
			int clamped = (requested_max < 1) ? 1 : ((requested_max > upper) ? upper : requested_max);

			if (clamped != requested_max) {
				LOG_INFO("AnthropicAPI", "Clamped max_tokens from " + std::to_string(requested_max)
					+ " to " + std::to_string(clamped) + " (context_window="
					+ std::to_string(context_window) + ", input_est="
					+ std::to_string(estimated_input) + ")");
				openai_req["max_tokens"] = clamped;
			}
			else {
				openai_req["max_tokens"] = requested_max;
			}
		}

		std::string body = apply_compatibility_layer(openai_req.dump(), fallback_model);

		size_t max_key_retries = key_manager.get_keys_count();
		if (max_key_retries == 0) {
			LOG_ERROR("AnthropicAPI", "No API keys configured.");
			res.status = 500;
			res.set_content(make_anthropic_error("api_error", "No API keys configured").dump(), "application/json");
			return;
		}

		backoff_manager.acquire();
		struct ScopeRelease {
			ClientSideBackoff& b;
			~ScopeRelease() { b.release(); }
		} scope_release{ backoff_manager };

		for (size_t attempt = 0; attempt < max_key_retries; ++attempt) {
			std::string key = key_manager.get_key();
			if (key.empty()) {
				res.status = 503;
				res.set_content(make_anthropic_error("overloaded_error", "Keys cooling down").dump(), "application/json");
				return;
			}

			std::string upstream_url = NVIDIA_BASE_URL + "/chat/completions";
			std::string masked_key = (key.size() > 6) ? key.substr(key.size() - 6) : key;
			LOG_DEBUG("AnthropicAPI", "Routing request with Key ..." + masked_key + " (Attempt " + std::to_string(attempt + 1) + ")");

			CURL* curl = curl_easy_init();
			if (!curl) {
				key_manager.mark_failed(key, 500);
				continue;
			}

			struct curl_slist* headers_list = nullptr;
			headers_list = curl_slist_append(headers_list, "Content-Type: application/json");
			headers_list = curl_slist_append(headers_list, "Accept-Encoding: identity");
			headers_list = curl_slist_append(headers_list, "User-Agent: nim-proxy-cpp/1.0");
			headers_list = curl_slist_append(headers_list, "X-Source: nim-proxy-cpp");
			std::string auth_h = "Authorization: Bearer " + key;
			headers_list = curl_slist_append(headers_list, auth_h.c_str());

			curl_easy_setopt(curl, CURLOPT_URL, upstream_url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());

			configure_curl_network_stability(curl);

			auto ctx = std::make_shared<ProxyContext>();
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, custom_header_callback);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx.get());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, custom_write_callback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());

			auto start_time = std::chrono::steady_clock::now();
			auto curl_thread = std::make_shared<std::thread>(run_curl_request, curl, ctx.get());

			{
				std::unique_lock<std::mutex> lock(ctx->header_mtx);
				ctx->header_cv.wait(lock, [&]() { return ctx->headers_done.load(); });
			}

			auto ttfb = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

			if (ctx->curl_failed) {
				LOG_WARN("AnthropicAPI", "Upstream network transfer failed: " + ctx->curl_error_msg + " (Retrying attempt " + std::to_string(attempt + 1) + ")");
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 1000)));
				continue;
			}

			int status_code = ctx->http_status.load();
			LOG_DEBUG("AnthropicAPI", "Upstream response headers received | Status: " + std::to_string(status_code) + " | TTFB: " + std::to_string(ttfb) + "ms");

			if (status_code == 429 || status_code == 401 || status_code == 403 || status_code == 502 || status_code == 503) {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);
				key_manager.mark_failed(key, status_code);
				continue;
			}

			if (status_code != 200) {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				LOG_WARN("AnthropicAPI", "Non-200 upstream error: " + std::to_string(status_code));
				std::string err_type = (status_code == 429) ? "rate_limit_error" : ((status_code >= 500) ? "api_error" : "invalid_request_error");

				std::string err_msg = "Upstream returned HTTP " + std::to_string(status_code);
				if (!ctx->full_body_buffer.empty()) {
					try {
						auto nim_err = nlohmann::json::parse(ctx->full_body_buffer);
						if (nim_err.contains("error")) {
							if (nim_err["error"].is_string()) {
								err_msg = nim_err["error"].get<std::string>();
							} else if (nim_err["error"].is_object() && nim_err["error"].contains("message") && nim_err["error"]["message"].is_string()) {
								err_msg = nim_err["error"]["message"].get<std::string>();
							}
						} else if (nim_err.contains("message") && nim_err["message"].is_string()) {
							err_msg = nim_err["message"].get<std::string>();
						}
					} catch (...) {
						if (ctx->full_body_buffer.size() < 2048) {
							err_msg = ctx->full_body_buffer;
						}
					}
				}

			res.status = status_code;
			res.set_content(make_anthropic_error(err_type, err_msg).dump(), "application/json");
			return;
		}

		key_manager.mark_success(key);

		if (ctx->is_stream) {
				auto lstate = std::make_shared<LambdaState>();
				lstate->curl_thread = curl_thread;
				lstate->ctx = ctx;
				lstate->curl = curl;
				lstate->headers_list = headers_list;
				lstate->anthropic_state.model = fallback_model;
				lstate->anthropic_state.input_tokens = estimate_input_tokens(*anthropic_json);

				res.set_chunked_content_provider(
					"text/event-stream",
					[lstate](size_t offset, httplib::DataSink& sink) {
						if (!sink.is_writable()) {
							LOG_WARN("Stream", "Downstream client aborted connection mid-stream.");
							return false;
						}

						std::string chunk;
						if (lstate->ctx->chunk_queue.pop_timeout(chunk, std::chrono::seconds(5))) {
							lstate->buffer += chunk;
							lstate->total_bytes += chunk.size();

							size_t pos;
							while ((pos = lstate->buffer.find('\n')) != std::string::npos) {
								std::string line = lstate->buffer.substr(0, pos);
								lstate->buffer.erase(0, pos + 1);

								std::string anthropic_sse = process_line_for_anthropic_stream(line, lstate->anthropic_state);
								if (!anthropic_sse.empty()) {
									sink.write(anthropic_sse.data(), anthropic_sse.size());
									lstate->lines_emitted++;
								}
							}
							return true;
						}
						else {
							if (!lstate->ctx->chunk_queue.is_finished()) {
								const char* ping = "event: ping\ndata: {\"type\": \"ping\"}\n\n";
								sink.write(ping, std::strlen(ping));
								return true;
							}

							if (!lstate->buffer.empty()) {
								std::string anthropic_sse = process_line_for_anthropic_stream(lstate->buffer, lstate->anthropic_state);
								if (!anthropic_sse.empty()) {
									sink.write(anthropic_sse.data(), anthropic_sse.size());
									lstate->lines_emitted++;
								}
								lstate->buffer.clear();
							}
							sink.done();
							return false;
						}
					}
				);
			}
			else {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				try {
					nlohmann::json openai_res = nlohmann::json::parse(ctx->full_body_buffer);
					nlohmann::json anthropic_res = convert_openai_to_anthropic_response(openai_res, fallback_model);
					res.status = 200;
					res.set_content(anthropic_res.dump(), "application/json");
					LOG_INFO("AnthropicAPI", "Successfully served non-streaming response.");
				}
				catch (...) {
					LOG_ERROR("AnthropicAPI", "Failed converting OpenAI payload to Anthropic format.");
					res.status = 500;
					res.set_content(make_anthropic_error("api_error", "Failed to transform response").dump(), "application/json");
				}
			}
			return;
		}

		LOG_ERROR("AnthropicAPI", "All backend key retry attempts failed.");
		res.status = 502;
		res.set_content(make_anthropic_error("overloaded_error", "All backends failed").dump(), "application/json");
	};

	svr.Post("/v1/messages", handle_anthropic_messages);
	svr.Post("/messages", handle_anthropic_messages);

	// --- GENERIC OPENAI WILDCARD ROUTE HANDLER ---
	auto handle_openai_proxy = [&](const httplib::Request& req, httplib::Response& res) {
		std::string path = req.path;
		if (path.rfind("/v1/", 0) == 0) {
			path = path.substr(4);
		}

		// httplib 0.43.3 dispatches wildcard routes before exact-match routes
		// for POST requests. Guard against this: if the wildcard catches a
		// /messages request, delegate to the dedicated Anthropic handler.
		if (path == "messages") {
			LOG_INFO("AnthropicAPI", "Delegating misrouted /messages request to Anthropic handler.");
			handle_anthropic_messages(req, res);
			return;
		}

		LOG_INFO("OpenAIAPI", "Request from " + req.remote_addr + " | Path: /" + path);
		std::string body = apply_compatibility_layer(req.body);

		size_t max_key_retries = key_manager.get_keys_count();
		if (max_key_retries == 0) {
			LOG_ERROR("OpenAIAPI", "No API keys configured.");
			res.status = 500;
			res.set_content("{\"error\": \"No API keys configured\"}", "application/json");
			return;
		}

		backoff_manager.acquire();
		struct ScopeRelease {
			ClientSideBackoff& b;
			~ScopeRelease() { b.release(); }
		} scope_release{ backoff_manager };

		for (size_t attempt = 0; attempt < max_key_retries; ++attempt) {
			std::string key = key_manager.get_key();
			if (key.empty()) {
				res.status = 503;
				res.set_content("{\"error\": \"Keys cooling down\"}", "application/json");
				return;
			}

			std::string upstream_url = NVIDIA_BASE_URL + "/" + path;
			std::string masked_key = (key.size() > 6) ? key.substr(key.size() - 6) : key;
			LOG_DEBUG("OpenAIAPI", "Routing request with Key ..." + masked_key + " (Attempt " + std::to_string(attempt + 1) + ")");

			CURL* curl = curl_easy_init();
			if (!curl) {
				key_manager.mark_failed(key, 500);
				continue;
			}

			struct curl_slist* headers_list = nullptr;
			headers_list = curl_slist_append(headers_list, "Content-Type: application/json");
			headers_list = curl_slist_append(headers_list, "Accept-Encoding: identity");
			headers_list = curl_slist_append(headers_list, "User-Agent: nim-proxy-cpp/1.0");
			headers_list = curl_slist_append(headers_list, "X-Source: nim-proxy-cpp");
			std::string auth_h = "Authorization: Bearer " + key;
			headers_list = curl_slist_append(headers_list, auth_h.c_str());

			curl_easy_setopt(curl, CURLOPT_URL, upstream_url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

			if (req.method == "POST") {
				curl_easy_setopt(curl, CURLOPT_POST, 1L);
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
			}
			else if (req.method == "GET") {
				curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
			}
			else {
				curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
			}

			configure_curl_network_stability(curl);

			auto ctx = std::make_shared<ProxyContext>();
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, custom_header_callback);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx.get());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, custom_write_callback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());

			auto start_time = std::chrono::steady_clock::now();
			auto curl_thread = std::make_shared<std::thread>(run_curl_request, curl, ctx.get());

			{
				std::unique_lock<std::mutex> lock(ctx->header_mtx);
				ctx->header_cv.wait(lock, [&]() { return ctx->headers_done.load(); });
			}

			auto ttfb = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();

			if (ctx->curl_failed) {
				LOG_WARN("OpenAIAPI", "Upstream network transfer failed: " + ctx->curl_error_msg + " (Retrying attempt " + std::to_string(attempt + 1) + ")");
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 1000)));
				continue;
			}

			int status_code = ctx->http_status.load();
			LOG_DEBUG("OpenAIAPI", "Upstream response headers received | Status: " + std::to_string(status_code) + " | TTFB: " + std::to_string(ttfb) + "ms");

			if (status_code == 429 || status_code == 401 || status_code == 403 || status_code == 502 || status_code == 503) {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);
				key_manager.mark_failed(key, status_code);
				continue;
			}

			if (status_code != 200) {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

			LOG_WARN("OpenAIAPI", "Non-200 upstream error: " + std::to_string(status_code));
			res.status = status_code;
			res.set_content(ctx->full_body_buffer, ctx->content_type);
			return;
		}

		key_manager.mark_success(key);

		if (ctx->is_stream) {
				auto lstate = std::make_shared<LambdaState>();
				lstate->curl_thread = curl_thread;
				lstate->ctx = ctx;
				lstate->curl = curl;
				lstate->headers_list = headers_list;

				res.set_chunked_content_provider(
					ctx->content_type,
					[lstate](size_t offset, httplib::DataSink& sink) {
						if (!sink.is_writable()) {
							LOG_WARN("Stream", "Downstream client aborted connection mid-stream.");
							return false;
						}

						std::string chunk;
						if (lstate->ctx->chunk_queue.pop_timeout(chunk, std::chrono::seconds(5))) {
							sink.write(chunk.data(), chunk.size());
							lstate->total_bytes += chunk.size();
							lstate->lines_emitted++;
							return true;
						}
						else {
							if (!lstate->ctx->chunk_queue.is_finished()) {
								const char* ping = ": ping\n\n";
								sink.write(ping, std::strlen(ping));
								return true;
							}
							sink.done();
							return false;
						}
					}
				);
			}
			else {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				res.set_content(ctx->full_body_buffer, ctx->content_type);
				LOG_INFO("OpenAIAPI", "Successfully served non-streaming response.");
			}
			return;
		}

		LOG_ERROR("OpenAIAPI", "All backend key retry attempts failed.");
		res.status = 502;
		res.set_content("{\"error\": \"All connection attempts failed\"}", "application/json");
	};

	svr.Post("/v1/chat/completions", handle_openai_proxy);
	svr.Post("/v1/completions", handle_openai_proxy);
	svr.Post("/v1/embeddings", handle_openai_proxy);
	svr.Post("/v1/chat", handle_openai_proxy);
	svr.Post("/v1/edits", handle_openai_proxy);
	svr.Post("/v1/images/generations", handle_openai_proxy);
	svr.Post("/v1/audio/transcriptions", handle_openai_proxy);
	svr.Post("/v1/audio/translations", handle_openai_proxy);
	svr.Post("/v1/moderations", handle_openai_proxy);
	svr.Post("/v1/rerank", handle_openai_proxy);
	svr.Get("/v1/chat/completions", handle_openai_proxy);
	svr.Post("/v1/(.*)", handle_openai_proxy);
	svr.Get("/v1/(.*)", handle_openai_proxy);

	LOG_INFO("Server", "NVIDIA NIM Proxy listening on http://127.0.0.1:8100");
	LOG_INFO("Server", " - Health Endpoint:    http://127.0.0.1:8100/health");
	LOG_INFO("Server", " - Keys Endpoint:      http://127.0.0.1:8100/v1/keys");
	LOG_INFO("Server", " - Models Endpoint:    http://127.0.0.1:8100/v1/models");
	LOG_INFO("Server", " - OpenAI Endpoint:    http://127.0.0.1:8100/v1/chat/completions");
	LOG_INFO("Server", " - Anthropic Endpoint: http://127.0.0.1:8100/v1/messages");

	svr.listen("127.0.0.1", 8100);

	shutdown.store(true);
	if (sync_thread.joinable()) sync_thread.join();
	curl_global_cleanup();
	return 0;
}

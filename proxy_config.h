#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>
#include <thread>
#include <chrono>
#include <regex>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "logger.h"

// ============================================================================
// Constants
// ============================================================================

const std::string NVIDIA_BASE_URL = []() -> std::string {
	const char* env_url = std::getenv("NVIDIA_BASE_URL");
	if (env_url && std::strlen(env_url) > 0) return std::string(env_url);
	const char* nim_url = std::getenv("NIM_BASE_URL");
	if (nim_url && std::strlen(nim_url) > 0) return std::string(nim_url);
	return std::string("https://integrate.api.nvidia.com/v1");
}();

// ============================================================================
// Thread-Safe Synchronization
// ============================================================================

template <typename T>
class SafeQueue {
private:
	std::queue<T> q;
	std::mutex mtx;
	std::condition_variable cv;
	bool finished = false;

public:
	void push(T val) {
		std::lock_guard<std::mutex> lock(mtx);
		q.push(std::move(val));
		cv.notify_one();
	}

	void finish() {
		std::lock_guard<std::mutex> lock(mtx);
		finished = true;
		cv.notify_all();
	}

	bool pop_timeout(T& val, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mtx);
		if (!cv.wait_for(lock, timeout, [this]() { return !q.empty() || finished; })) {
			return false;
		}
		if (q.empty()) return false;
		val = std::move(q.front());
		q.pop();
		return true;
	}

	bool is_finished() {
		std::lock_guard<std::mutex> lock(mtx);
		return finished && q.empty();
	}
};

class Semaphore {
private:
	std::mutex mtx;
	std::condition_variable cv;
	int count;

public:
	Semaphore(int count_ = 0) : count(count_) {}

	void notify() {
		std::unique_lock<std::mutex> lock(mtx);
		count++;
		cv.notify_one();
	}

	void wait() {
		std::unique_lock<std::mutex> lock(mtx);
		while (count == 0) {
			cv.wait(lock);
		}
		count--;
	}
};

// ============================================================================
// Network Types
// ============================================================================

struct CurlBuffer {
	std::string data;
};

inline size_t write_buffer_callback(char* ptr, size_t size, size_t nmemb, void* userp) {
	auto* buf = static_cast<CurlBuffer*>(userp);
	size_t total_size = size * nmemb;
	buf->data.append(ptr, total_size);
	return total_size;
}

struct ProxyContext {
	std::atomic<int> http_status{ 0 };
	std::atomic<bool> headers_done{ false };
	std::mutex header_mtx;
	std::condition_variable header_cv;

	SafeQueue<std::string> chunk_queue;
	std::string full_body_buffer;
	std::string content_type = "application/json";
	bool is_stream = false;
	std::atomic<bool> curl_failed{ false };
	std::string curl_error_msg;
	std::atomic<bool> client_disconnected{ false };
};

inline size_t custom_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
	auto* ctx = static_cast<ProxyContext*>(userdata);
	size_t total_size = size * nitems;
	std::string h(buffer, total_size);
	if (h.size() >= 2 && h.substr(h.size() - 2) == "\r\n") {
		h = h.substr(0, h.size() - 2);
	}

	if (h.rfind("HTTP/", 0) == 0) {
		std::regex status_regex(R"(HTTP/[^\s]+\s+(\d+))");
		std::smatch match;
		if (std::regex_search(h, match, status_regex)) {
			ctx->http_status = std::stoi(match[1].str());
		}
	}
	else if (!h.empty()) {
		size_t colon = h.find(':');
		if (colon != std::string::npos) {
			std::string key = h.substr(0, colon);
			std::string val = h.substr(colon + 1);
			key.erase(0, key.find_first_not_of(" \t"));
			key.erase(key.find_last_not_of(" \t") + 1);
			val.erase(0, val.find_first_not_of(" \t"));
			val.erase(val.find_last_not_of(" \t") + 1);

			std::string key_lower = key;
			std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), [](unsigned char c) { return std::tolower(c); });

			if (key_lower == "content-type") {
				ctx->content_type = val;
				if (val.find("text/event-stream") != std::string::npos) {
					ctx->is_stream = true;
				}
			}
		}
	}
	else {
		std::unique_lock<std::mutex> lock(ctx->header_mtx);
		ctx->headers_done = true;
		ctx->header_cv.notify_one();
	}
	return total_size;
}

inline size_t custom_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* ctx = static_cast<ProxyContext*>(userdata);
	if (ctx->client_disconnected.load()) {
		return 0;
	}
	size_t total_size = size * nmemb;
	std::string chunk(ptr, total_size);

	if (ctx->is_stream) {
		ctx->chunk_queue.push(chunk);
	}
	else {
		ctx->full_body_buffer.append(chunk);
	}
	return total_size;
}

inline void run_curl_request(CURL* curl, ProxyContext* ctx) {
	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		ctx->curl_failed = true;
		ctx->curl_error_msg = curl_easy_strerror(res);
		std::unique_lock<std::mutex> lock(ctx->header_mtx);
		ctx->headers_done = true;
		ctx->header_cv.notify_one();
	}
	ctx->chunk_queue.finish();
}

inline void configure_curl_network_stability(CURL* curl) {
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 300L);   // allow very slow models (NIM free tier)
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
	curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 900L);           // 15 minutes for slow reasoning models
}

// ============================================================================
// Anthropic Stream State
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
		msg_id = "msg_" + std::to_string(std::rand() % 1000000);
	}
};

// ============================================================================
// Lambda State (Streaming)
// ============================================================================

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

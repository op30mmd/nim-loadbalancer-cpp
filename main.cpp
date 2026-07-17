#define _CRT_SECURE_NO_WARNINGS // Suppress MSVC deprecation warnings for getenv/C-functions

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <chrono>
#include <thread>
#include <regex>
#include <sstream>
#include <fstream>
#include <memory>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <future>

// Windows Platform Definitions
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")   // Links Winsock2 library automatically on MSVC
#pragma comment(lib, "crypt32.lib")  // Links Cryptographic services
#pragma comment(lib, "bcrypt.lib")   // Links Cryptography API: Next Generation (CNG)
#endif

// Dependencies
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

const std::string NVIDIA_BASE_URL = "https://integrate.api.nvidia.com/v1";

// ============================================================================
// Thread-Safe Utilities & Synchronizers
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

	bool pop(T& val) {
		std::unique_lock<std::mutex> lock(mtx);
		while (q.empty() && !finished) {
			cv.wait(lock);
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
// Core Management Classes
// ============================================================================

struct KeyItem {
	std::string key;
	std::chrono::steady_clock::time_point cooldown_until;
};

class KeyManager {
private:
	std::vector<KeyItem> keys;
	int default_cooldown;
	size_t index = 0;
	std::mutex mtx;

public:
	KeyManager(const std::vector<std::string>& raw_keys, int cooldown = 60)
		: default_cooldown(cooldown) {
		for (const auto& k : raw_keys) {
			keys.push_back({ k, std::chrono::steady_clock::time_point() });
		}
	}

	std::string get_key() {
		std::lock_guard<std::mutex> lock(mtx);
		if (keys.empty()) return "";
		auto now = std::chrono::steady_clock::now();
		size_t total = keys.size();
		for (size_t i = 0; i < total; ++i) {
			auto& candidate = keys[index];
			index = (index + 1) % total;
			if (candidate.cooldown_until <= now) {
				return candidate.key;
			}
		}
		return "";
	}

	void mark_failed(const std::string& key, int status_code) {
		std::lock_guard<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		for (auto& item : keys) {
			if (item.key == key) {
				int duration = (status_code == 429) ? default_cooldown * 2 : default_cooldown;
				item.cooldown_until = now + std::chrono::seconds(duration);
				std::cout << "[DEBUG] [KeyManager] Key ..."
					<< (key.size() > 6 ? key.substr(key.size() - 6) : key)
					<< " flagged on HTTP " << status_code << ". Cooldown set to " << duration << "s.\n";
				break;
			}
		}
	}

	size_t get_keys_count() {
		std::lock_guard<std::mutex> lock(mtx);
		return keys.size();
	}

	std::string get_first_key() {
		std::lock_guard<std::mutex> lock(mtx);
		if (keys.empty()) return "";
		return keys[0].key;
	}
};

class ClientSideBackoff {
private:
	Semaphore sem;
	std::chrono::steady_clock::time_point last_request_time;
	double min_interval;
	std::chrono::steady_clock::time_point global_backoff_until;
	std::mutex mtx;

public:
	ClientSideBackoff(int max_concurrent = 2, double interval = 1.0)
		: sem(max_concurrent), min_interval(interval) {
		last_request_time = std::chrono::steady_clock::now() - std::chrono::milliseconds(2000);
		global_backoff_until = std::chrono::steady_clock::now();
	}

	void acquire() {
		sem.wait();
		std::unique_lock<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		if (now < global_backoff_until) {
			auto wait_dur = std::chrono::duration_cast<std::chrono::milliseconds>(global_backoff_until - now);
			std::cout << "[DEBUG] [Backoff] Protective global backoff active. Throttling request for "
				<< wait_dur.count() / 1000.0 << "s...\n";
			std::this_thread::sleep_for(wait_dur);
			now = std::chrono::steady_clock::now();
		}

		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time).count() / 1000.0;
		if (elapsed < min_interval) {
			double wait_sec = min_interval - elapsed;
			std::cout << "[DEBUG] [Backoff] Throttling sequential request. Waiting " << wait_sec << "s...\n";
			std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(wait_sec * 1000)));
		}
		last_request_time = std::chrono::steady_clock::now();
	}

	void release() {
		sem.notify();
	}

	void trigger_global_backoff(double duration) {
		std::unique_lock<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		auto target = now + std::chrono::milliseconds(static_cast<long long>(duration * 1000));
		if (target > global_backoff_until) {
			global_backoff_until = target;
			std::cout << "[DEBUG] [Backoff] Global IP-limit protection triggered. All new requests paused for "
				<< duration << "s.\n";
		}
	}
};

class ModelCache {
private:
	std::mutex mtx;
	double ttl;
	std::string cached_data;
	std::chrono::steady_clock::time_point expiry;
	bool has_cache = false;

public:
	ModelCache(double ttl_sec = 3600) : ttl(ttl_sec) {}

	std::string get() {
		std::lock_guard<std::mutex> lock(mtx);
		if (has_cache && std::chrono::steady_clock::now() < expiry) {
			return cached_data;
		}
		return "";
	}

	void set(const std::string& data) {
		std::lock_guard<std::mutex> lock(mtx);
		cached_data = data;
		expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(ttl * 1000));
		has_cache = true;
	}
};

// ============================================================================
// JSON Repair & Processing Layer
// ============================================================================

std::string repair_json_string(std::string s) {
	s.erase(0, s.find_first_not_of(" \t\n\r"));
	s.erase(s.find_last_not_of(" \t\n\r") + 1);
	if (s.empty()) return s;

	std::vector<char> stack;
	bool in_string = false;
	bool escape = false;

	for (size_t i = 0; i < s.length(); ++i) {
		char ch = s[i];
		if (escape) {
			escape = false;
			continue;
		}
		if (ch == '\\') {
			escape = true;
			continue;
		}
		if (ch == '"') {
			in_string = !in_string;
			continue;
		}
		if (!in_string) {
			if (ch == '{' || ch == '[') {
				stack.push_back(ch);
			}
			else if (ch == '}' || ch == ']') {
				if (!stack.empty()) {
					stack.pop_back();
				}
			}
		}
	}

	if (in_string) {
		s += '"';
	}

	while (!stack.empty()) {
		char item = stack.back();
		stack.pop_back();
		if (item == '{') s += '}';
		else if (item == '[') s += ']';
	}
	return s;
}

struct StreamState {
	bool thinking_started = false;
	bool thinking_finished = false;
};

std::string process_and_repair_line(const std::string& line, StreamState& state) {
	std::string line_stripped = line;
	line_stripped.erase(0, line_stripped.find_first_not_of(" \t\n\r"));
	line_stripped.erase(line_stripped.find_last_not_of(" \t\n\r") + 1);
	if (line_stripped.empty()) {
		return line;
	}

	if (line_stripped.rfind("data: ", 0) == 0) {
		std::string json_str = line_stripped.substr(6);
		json_str.erase(0, json_str.find_first_not_of(" \t\n\r"));
		if (json_str == "[DONE]") {
			return line;
		}

		auto process_json_obj = [&](nlohmann::json& parsed, bool& is_ok) -> std::string {
			is_ok = false;
			if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
				auto& choice = parsed["choices"][0];
				if (choice.contains("delta") && choice["delta"].is_object()) {
					auto& delta = choice["delta"];

					std::string content = "";
					if (delta.contains("content") && delta["content"].is_string()) {
						content = delta["content"].get<std::string>();
					}

					std::string reasoning = "";
					if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
						reasoning = delta["reasoning_content"].get<std::string>();
					}
					else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
						reasoning = delta["reasoning"].get<std::string>();
					}

					std::string finish_reason = "";
					if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
						finish_reason = choice["finish_reason"].get<std::string>();
					}

					if (!reasoning.empty()) {
						std::cout << "[DEBUG] [Stream Output] Thinking Token: " << reasoning << "\n";
						if (!state.thinking_started) {
							delta["content"] = "<think>\n" + reasoning;
							state.thinking_started = true;
						}
						else {
							delta["content"] = reasoning;
						}
					}
					else if (!content.empty()) {
						std::cout << "[DEBUG] [Stream Output] Content Token: " << content << "\n";
						if (state.thinking_started && !state.thinking_finished) {
							delta["content"] = "\n</think>\n" + content;
							state.thinking_finished = true;
						}
					}

					if (!finish_reason.empty()) {
						std::cout << "[DEBUG] [Stream Output] Stream Finished. Reason: " << finish_reason << "\n";
						if (state.thinking_started && !state.thinking_finished) {
							std::string existing_content = "";
							if (delta.contains("content") && delta["content"].is_string()) {
								existing_content = delta["content"].get<std::string>();
							}
							delta["content"] = existing_content + "\n</think>\n";
							state.thinking_finished = true;
						}
					}

					delta.erase("reasoning_content");
					delta.erase("reasoning");
					is_ok = true;
					return "data: " + parsed.dump();
				}
			}
			else if (parsed.contains("error")) {
				std::cout << "[DEBUG] [Stream Output] Upstream Error detected in JSON: " << parsed["error"].dump() << "\n";
			}
			return "";
			};

		try {
			nlohmann::json parsed = nlohmann::json::parse(json_str);
			bool ok = false;
			std::string out = process_json_obj(parsed, ok);
			if (ok) return out;
		}
		catch (const nlohmann::json::parse_error& exc) {
			std::cout << "[DEBUG] [JSON Repair] Malformed JSON payload detected. Attempting repair...\n";
			std::string repaired = repair_json_string(json_str);
			try {
				nlohmann::json parsed = nlohmann::json::parse(repaired);
				bool ok = false;
				std::string out = process_json_obj(parsed, ok);
				if (ok) return out;
			}
			catch (const std::exception& e) {
				std::cout << "[DEBUG] [JSON Repair] Repair attempt failed. Outputting raw string.\n";
			}
		}
	}
	return line;
}

// ============================================================================
// Config Auto-Sync Layer
// ============================================================================

bool file_exists(const std::string& path) {
	std::ifstream f(path.c_str());
	return f.good();
}

std::string get_home_dir() {
#ifdef _WIN32
	// Windows fallback: checks USERPROFILE or joins HOMEDRIVE + HOMEPATH
	const char* home = std::getenv("USERPROFILE");
	if (!home) {
		const char* drive = std::getenv("HOMEDRIVE");
		const char* path = std::getenv("HOMEPATH");
		if (drive && path) {
			return std::string(drive) + std::string(path);
		}
	}
	return home ? std::string(home) : "";
#else
	const char* home = std::getenv("HOME");
	return home ? std::string(home) : "";
#endif
}

std::string find_config_file() {
	std::vector<std::string> names = { "kilo.jsonc", "kilo.json", "opencode.jsonc", "opencode.json" };
	for (const auto& name : names) {
		if (file_exists(name)) return name;
	}
	std::string home = get_home_dir();
	if (!home.empty()) {
		std::vector<std::string> global_paths = {
			home + "/.config/kilo/kilo.jsonc",
			home + "/.config/kilo/kilo.json",
			home + "/.config/opencode/opencode.jsonc",
			home + "/.config/opencode/opencode.json",
			// Windows AppData native mapping support
			home + "/AppData/Roaming/kilo/kilo.jsonc",
			home + "/AppData/Roaming/kilo/kilo.json",
			home + "/AppData/Roaming/opencode/opencode.jsonc",
			home + "/AppData/Roaming/opencode/opencode.json"
		};
		for (const auto& path : global_paths) {
			if (file_exists(path)) return path;
		}
	}
	return "";
}

nlohmann::json parse_jsonc(const std::string& filepath) {
	std::ifstream file(filepath);
	if (!file.is_open()) {
		throw std::runtime_error("Cannot open file: " + filepath);
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string content = buffer.str();

	// Uses the default ECMAScript engine to support lazy matching safely
	content = std::regex_replace(content, std::regex(R"(/\*[\s\S]*?\*/)"), "");
	content = std::regex_replace(content, std::regex(R"(//.*?\n)"), "\n");
	content = std::regex_replace(content, std::regex(R"(,(\s*[\]}]))"), "$1");

	return nlohmann::json::parse(content);
}

void update_opencode_config(const std::vector<std::string>& models_list) {
	std::string config_path = find_config_file();
	if (config_path.empty()) {
		std::cout << "[Config Sync] Could not locate any configuration file.\n";
		return;
	}

	nlohmann::json config_data;
	try {
		config_data = parse_jsonc(config_path);
	}
	catch (const std::exception& e) {
		std::cout << "[Config Sync] Error reading/parsing config at " << config_path << ": " << e.what() << "\n";
		return;
	}

	if (!config_data.contains("provider")) {
		config_data["provider"] = nlohmann::json::object();
	}

	std::string provider_id = "nvidia";
	if (!config_data["provider"].contains(provider_id)) {
		config_data["provider"][provider_id] = {
			{"options", {{"baseURL", "http://127.0.0.1:8100/v1"}}}
		};
	}
	else {
		if (!config_data["provider"][provider_id].contains("options")) {
			config_data["provider"][provider_id]["options"] = nlohmann::json::object();
		}
		config_data["provider"][provider_id]["options"]["baseURL"] = "http://127.0.0.1:8100/v1";
	}

	nlohmann::json models_dict = nlohmann::json::object();
	for (const auto& model_id : models_list) {
		size_t slash_pos = model_id.find_last_of('/');
		std::string raw_name = (slash_pos == std::string::npos) ? model_id : model_id.substr(slash_pos + 1);
		for (auto& c : raw_name) {
			if (c == '-') c = ' ';
		}
		bool cap = true;
		for (auto& c : raw_name) {
			if (std::isspace(static_cast<unsigned char>(c))) {
				cap = true;
			}
			else if (cap) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				cap = false;
			}
			else {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
		}
		models_dict[model_id] = { {"name", raw_name} };
	}

	config_data["provider"][provider_id]["models"] = models_dict;

	try {
		std::ofstream out(config_path);
		out << config_data.dump(2);
		std::cout << "[Config Sync] Automatically wrote " << models_list.size() << " models to " << config_path << ".\n";
	}
	catch (const std::exception& e) {
		std::cout << "[Config Sync] Failed writing config back: " << e.what() << "\n";
	}
}

// ============================================================================
// Libcurl Write Buffers & Callbacks
// ============================================================================

struct CurlBuffer {
	std::string data;
};

size_t write_buffer_callback(char* ptr, size_t size, size_t nmemb, void* userp) {
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
	std::string content_type = "text/plain";
	std::vector<std::pair<std::string, std::string>> response_headers;
	bool is_stream = false;
	std::atomic<bool> curl_failed{ false };
	std::string curl_error_msg;
	std::atomic<bool> client_disconnected{ false };
};

size_t custom_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
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
			if (key_lower != "content-length" && key_lower != "content-encoding" &&
				key_lower != "transfer-encoding" && key_lower != "connection" &&
				key_lower != "keep-alive") {
				ctx->response_headers.push_back({ key, val });
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

size_t custom_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* ctx = static_cast<ProxyContext*>(userdata);
	if (ctx->client_disconnected.load()) {
		return 0; // Aborts curl_easy_perform if client dropped out
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

void run_curl_request(CURL* curl, ProxyContext* ctx) {
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

// ============================================================================
// Sync Task & Request Processing Helpers
// ============================================================================

void run_sync_config_task(KeyManager& key_manager, std::atomic<bool>& shutdown) {
	std::this_thread::sleep_for(std::chrono::seconds(1));
	if (shutdown.load()) return;

	std::string key = key_manager.get_first_key();
	if (key.empty()) return;

	CURL* curl = curl_easy_init();
	if (!curl) return;

	std::string url = "https://integrate.api.nvidia.com/v1/models";
	struct curl_slist* headers_list = nullptr;
	std::string auth_header = "Authorization: Bearer " + key;
	headers_list = curl_slist_append(headers_list, auth_header.c_str());

	CurlBuffer write_buf;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_buffer_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_buf);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	}

	curl_slist_free_all(headers_list);
	curl_easy_cleanup(curl);

	if (http_code == 200) {
		try {
			auto models_json = nlohmann::json::parse(write_buf.data);
			std::vector<std::string> models_list;
			if (models_json.contains("data") && models_json["data"].is_array()) {
				for (const auto& item : models_json["data"]) {
					if (item.contains("id") && item["id"].is_string()) {
						models_list.push_back(item["id"].get<std::string>());
					}
				}
			}
			if (!models_list.empty()) {
				update_opencode_config(models_list);
			}
		}
		catch (const std::exception& e) {
			std::cout << "[Config Sync] Failed parsing models list: " << e.what() << "\n";
		}
	}
	else {
		std::cout << "[Config Sync] Failed fetching models list, HTTP " << http_code << "\n";
	}
}

std::string apply_compatibility_layer(const std::string& body_str) {
	if (body_str.empty()) return body_str;
	try {
		nlohmann::json body_json = nlohmann::json::parse(body_str);

		std::string model_req = "unknown";
		if (body_json.contains("model") && body_json["model"].is_string()) {
			model_req = body_json["model"].get<std::string>();
		}
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
			std::cout << "[DEBUG] [Request] Auto-configured GLM-5 thinking parameters.\n";
		}
		else if (model_lower.find("deepseek-v4") != std::string::npos || model_lower.find("deepseek-r1") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			body_json["chat_template_kwargs"]["thinking"] = true;

			if (body_json.contains("tool_choice")) {
				body_json.erase("tool_choice");
				std::cout << "[DEBUG] [Request] Stripped 'tool_choice' parameter for DeepSeek compatibility.\n";
			}
			modified = true;
			std::cout << "[DEBUG] [Request] Auto-configured DeepSeek thinking parameters.\n";
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
			std::cout << "[DEBUG] [Request] Auto-configured Nemotron adaptive thinking parameters.\n";
		}
		else if (model_lower.find("qwen") != std::string::npos || model_lower.find("diffusiongemma") != std::string::npos) {
			body_json["chat_template_kwargs"]["enable_thinking"] = true;
			modified = true;
			std::cout << "[DEBUG] [Request] Auto-configured " << model_req << " thinking parameters.\n";
		}
		else if (!reasoning_effort.empty()) {
			if (reasoning_effort == "high" || reasoning_effort == "medium" || reasoning_effort == "max") {
				body_json["chat_template_kwargs"]["enable_thinking"] = true;
			}
			else if (reasoning_effort == "low") {
				body_json["chat_template_kwargs"]["enable_thinking"] = true;
				body_json["chat_template_kwargs"]["low_effort"] = true;
			}
			modified = true;
			std::cout << "[DEBUG] [Request] Mapped OpenAI reasoning_effort to NIM chat_template_kwargs.\n";
		}

		if (body_json["chat_template_kwargs"].empty()) {
			body_json.erase("chat_template_kwargs");
		}

		if (modified) {
			return body_json.dump();
		}
	}
	catch (const std::exception& e) {
		std::cout << "[DEBUG] [Request] Parsing failed in compatibility layer: " << e.what() << "\n";
	}
	return body_str;
}

// ============================================================================
// Core Routing Logics
// ============================================================================

struct LambdaState {
	std::string buffer = "";
	StreamState stream_state;
	size_t total_bytes = 0;
	size_t lines_emitted = 0;
	std::chrono::steady_clock::time_point stream_start = std::chrono::steady_clock::now();
	bool is_done_emitted = false;
	std::shared_ptr<std::thread> curl_thread;
	std::shared_ptr<ProxyContext> ctx;
	CURL* curl = nullptr;
	struct curl_slist* headers_list = nullptr;

	// RAII Destructor guarantees cleanup in all execution/abort paths
	~LambdaState() {
		std::cout << "[DEBUG] [RAII Cleanup] Destructor triggered...\n";

		// 1. Mark client as disconnected so the write callback in curl aborts immediately
		if (ctx) {
			ctx->client_disconnected = true;
			ctx->chunk_queue.finish(); // Unblocks pop() if waiting on condition variable
		}

		// 2. Safely join the curl thread
		if (curl_thread && curl_thread->joinable()) {
			std::cout << "[DEBUG] [RAII Cleanup] Joining curl background thread...\n";
			curl_thread->join();
		}

		// 3. Clean up libcurl resources
		if (curl) {
			std::cout << "[DEBUG] [RAII Cleanup] Releasing curl handle...\n";
			curl_easy_cleanup(curl);
		}
		if (headers_list) {
			std::cout << "[DEBUG] [RAII Cleanup] Releasing header list...\n";
			curl_slist_free_all(headers_list);
		}
	}
};

std::vector<std::string> load_api_keys() {
	std::vector<std::string> keys;
	const char* env_keys = std::getenv("NVIDIA_API_KEY");
	if (env_keys) {
		std::string s(env_keys);
		std::stringstream ss(s);
		std::string key;
		while (std::getline(ss, key, ',')) {
			key.erase(0, key.find_first_not_of(" \t"));
			key.erase(key.find_last_not_of(" \t") + 1);
			if (!key.empty()) {
				keys.push_back(key);
			}
		}
	}
	if (keys.empty()) {
		const char* single_key = std::getenv("NVIDIA_API_KEY");
		if (single_key) {
			std::string s(single_key);
			s.erase(0, s.find_first_not_of(" \t"));
			s.erase(s.find_last_not_of(" \t") + 1);
			if (!s.empty()) {
				keys.push_back(s);
			}
		}
	}
	return keys;
}

// ============================================================================
// Main Application Runner
// ============================================================================

int main() {
	curl_global_init(CURL_GLOBAL_ALL);
	std::srand(static_cast<unsigned>(std::time(nullptr)));

	auto keys = load_api_keys();
	std::cout << "[DEBUG] [Startup] Proxy loading. Registered API keys count: " << keys.size() << "\n";
	if (keys.empty()) {
		std::cout << "[DEBUG] [Startup] Warning: No keys loaded. Check environment variables.\n";
	}

	KeyManager key_manager(keys);
	ClientSideBackoff backoff_manager(2, 1.0);
	ModelCache model_cache(3600);

	std::atomic<bool> shutdown(false);
	std::thread sync_thread(run_sync_config_task, std::ref(key_manager), std::ref(shutdown));

	httplib::Server svr;

	// --- ENDPOINT: GET /v1/models ---
	svr.Get("/v1/models", [&](const httplib::Request& req, httplib::Response& res) {
		std::string cached = model_cache.get();
		if (!cached.empty()) {
			res.status = 200;
			res.set_content(cached, "application/json");
			return;
		}

		size_t max_key_retries = key_manager.get_keys_count();
		if (max_key_retries == 0) {
			res.status = 503;
			res.set_content("No keys configured.", "text/plain");
			return;
		}

		for (size_t i = 0; i < max_key_retries; ++i) {
			std::string key = key_manager.get_key();
			if (key.empty()) {
				res.status = 503;
				res.set_content("All keys are currently cooling down.", "text/plain");
				return;
			}

			std::cout << "[DEBUG] [Models] Querying catalog using key ..."
				<< (key.size() > 6 ? key.substr(key.size() - 6) : key) << "\n";

			CURL* curl = curl_easy_init();
			if (!curl) {
				key_manager.mark_failed(key, 500);
				continue;
			}

			std::string url = "https://integrate.api.nvidia.com/v1/models";
			struct curl_slist* headers_list = nullptr;
			std::string auth_header = "Authorization: Bearer " + key;
			headers_list = curl_slist_append(headers_list, auth_header.c_str());

			CurlBuffer write_buf;
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
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
				std::cout << "[DEBUG] [Network Error] Local connection failed: " << curl_easy_strerror(curl_res) << "\n";
				res.status = 503;
				res.set_content("Proxy unable to connect to NVIDIA. Verify your local internet routing.", "text/plain");
				return;
			}

			if (http_code == 429 || http_code == 401 || http_code == 403) {
				key_manager.mark_failed(key, http_code);
				continue;
			}

			if (http_code == 200) {
				model_cache.set(write_buf.data);
				res.status = 200;
				res.set_content(write_buf.data, "application/json");
				return;
			}

			res.status = http_code;
			res.set_content("Failed fetching models.", "text/plain");
			return;
		}

		res.status = 502;
		res.set_content("Failed to fetch models from NVIDIA.", "text/plain");
		});

	// --- GENERIC PROXY ---
	auto handle_proxy_v1 = [&](const std::string& path, const httplib::Request& req, httplib::Response& res) {
		std::string body = req.body;
		try {
			if (!body.empty()) {
				body = apply_compatibility_layer(body);
			}
		}
		catch (...) {}

		try {
			if (!body.empty()) {
				auto body_json = nlohmann::json::parse(body);

				std::string model_req = "unknown";
				if (body_json.contains("model") && body_json["model"].is_string()) {
					model_req = body_json["model"].get<std::string>();
				}

				size_t msg_count = 0;
				if (body_json.contains("messages") && body_json["messages"].is_array()) {
					msg_count = body_json["messages"].size();
				}

				std::vector<std::string> tool_names;
				if (body_json.contains("tools") && body_json["tools"].is_array()) {
					for (const auto& t : body_json["tools"]) {
						if (t.is_object() && t.contains("function") && t["function"].is_object()) {
							auto& f = t["function"];
							if (f.contains("name") && f["name"].is_string()) {
								tool_names.push_back(f["name"].get<std::string>());
							}
						}
					}
				}

				std::string tool_choice = "none";
				if (body_json.contains("tool_choice")) {
					if (body_json["tool_choice"].is_string()) {
						tool_choice = body_json["tool_choice"].get<std::string>();
					}
					else if (body_json["tool_choice"].is_object()) {
						tool_choice = body_json["tool_choice"].dump(); // Convert parsed object into standard string
					}
				}

				double payload_kb = body.size() / 1024.0;
				std::cout << "[DEBUG] [Request] Method: " << req.method << " | Path: /" << path << " | Payload: " << payload_kb << "KB\n"
					<< "       -> Target Model: " << model_req << "\n"
					<< "       -> Message History Length: " << msg_count << " items\n"
					<< "       -> Tools Provided: " << (tool_names.empty() ? "None" : "Available") << "\n"
					<< "       -> Tool Choice Directive: " << tool_choice << "\n";
			}
		}
		catch (const std::exception& e) {
			std::cout << "[DEBUG] [Request] Processing metrics failed: " << e.what() << "\n";
		}

		std::vector<std::string> upstream_headers;
		for (const auto& h : req.headers) {
			std::string key_lower = h.first;
			std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), [](unsigned char c) { return std::tolower(c); });
			if (key_lower == "host" || key_lower == "authorization" || key_lower == "content-length" || key_lower == "transfer-encoding") {
				continue;
			}
			upstream_headers.push_back(h.first + ": " + h.second);
		}
		upstream_headers.push_back("Accept-Encoding: identity");

		std::string query_str = "";
		if (!req.params.empty()) {
			query_str += "?";
			for (auto it = req.params.begin(); it != req.params.end(); ++it) {
				if (it != req.params.begin()) query_str += "&";
				query_str += it->first + "=" + it->second;
			}
		}

		size_t max_key_retries = key_manager.get_keys_count();
		if (max_key_retries == 0) {
			res.status = 500;
			res.set_content("No NIM API keys configured.", "text/plain");
			return;
		}

		backoff_manager.acquire();
		struct ScopeRelease {
			ClientSideBackoff& b;
			~ScopeRelease() { b.release(); }
		} scope_release{ backoff_manager };

		bool success = false;
		for (size_t attempt = 0; attempt < max_key_retries; ++attempt) {
			std::string key = key_manager.get_key();
			if (key.empty()) {
				res.status = 503;
				res.set_content("All configured keys are currently cooling down.", "text/plain");
				return;
			}

			std::string upstream_url = "https://integrate.api.nvidia.com/v1/" + path + query_str;
			std::cout << "[DEBUG] [KeyManager] Deploying Key ..."
				<< (key.size() > 6 ? key.substr(key.size() - 6) : key) << " for request.\n";

			CURL* curl = curl_easy_init();
			if (!curl) {
				key_manager.mark_failed(key, 500);
				continue;
			}

			struct curl_slist* headers_list = nullptr;
			for (const auto& h : upstream_headers) {
				headers_list = curl_slist_append(headers_list, h.c_str());
			}
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
				if (!body.empty()) {
					curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
					curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
				}
			}

			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

			auto ctx = std::make_shared<ProxyContext>();
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, custom_header_callback);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx.get());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, custom_write_callback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());

			auto curl_thread = std::make_shared<std::thread>(run_curl_request, curl, ctx.get());

			{
				std::unique_lock<std::mutex> lock(ctx->header_mtx);
				ctx->header_cv.wait(lock, [&]() { return ctx->headers_done.load(); });
			}

			int status_code = ctx->http_status.load();

			if (ctx->curl_failed) {
				std::cout << "[DEBUG] [Network Error] Connection to NVIDIA failed: " << ctx->curl_error_msg << "\n";
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				res.status = 503;
				res.set_content("Proxy unable to establish link to NVIDIA.", "text/plain");
				return;
			}

			if (status_code == 429) {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				key_manager.mark_failed(key, status_code);
				double cooldown_delay = 5.0 * std::pow(2.0, static_cast<double>(attempt)) + (((double)rand() / RAND_MAX) + 0.5);
				backoff_manager.trigger_global_backoff(cooldown_delay);

				std::cout << "[DEBUG] [Backoff] Retrying next available key in " << cooldown_delay << "s...\n";
				std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(cooldown_delay * 1000)));
				continue;
			}

			if (status_code == 401 || status_code == 403 || status_code == 502 || status_code == 503 || status_code == 504) {
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

				std::cout << "[DEBUG] [Response] Non-200 Error: " << ctx->full_body_buffer << "\n";
				res.status = status_code;
				res.set_content(ctx->full_body_buffer, ctx->content_type);
				return;
			}

			// Successfully set up! Apply proxy headers
			for (const auto& kv : ctx->response_headers) {
				res.set_header(kv.first, kv.second);
			}
			res.set_header("X-Accel-Buffering", "no");
			res.set_header("Cache-Control", "no-cache");
			res.set_header("Connection", "keep-alive");
			res.status = 200;

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
							std::cout << "[DEBUG] [Stream] Client aborted mid-stream. Lambda exiting...\n";
							return false; // Triggers lambda destruction and RAII cleanup
						}

						std::string chunk;
						if (lstate->ctx->chunk_queue.pop(chunk)) {
							lstate->total_bytes += chunk.size();
							lstate->buffer += chunk;

							size_t pos;
							while ((pos = lstate->buffer.find('\n')) != std::string::npos) {
								std::string line = lstate->buffer.substr(0, pos);
								lstate->buffer.erase(0, pos + 1);

								if (line.find("[DONE]") != std::string::npos) {
									lstate->is_done_emitted = true;
								}
								std::string processed_line = process_and_repair_line(line, lstate->stream_state);
								std::string out_line = processed_line + "\n";
								sink.write(out_line.data(), out_line.size());
								lstate->lines_emitted++;
							}
							return true;
						}
						else {
							// Queue finished naturally
							if (!lstate->buffer.empty()) {
								if (lstate->buffer.find("[DONE]") != std::string::npos) {
									lstate->is_done_emitted = true;
								}
								std::string processed_line = process_and_repair_line(lstate->buffer, lstate->stream_state);
								std::string out_line = processed_line + "\n";
								sink.write(out_line.data(), out_line.size());
								lstate->lines_emitted++;
								lstate->buffer.clear();
							}

							auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now() - lstate->stream_start).count() / 1000.0;
							std::cout << "[DEBUG] [Stream] Stream successfully terminated.\n"
								<< "       -> Stream Duration: " << duration << " seconds\n"
								<< "       -> Total Volume Transmitted: " << lstate->total_bytes / 1024.0 << " KB\n"
								<< "       -> Lines Emitted: " << lstate->lines_emitted << "\n";

							sink.done();
							return false; // Triggers lambda destruction and RAII cleanup
						}
					}
				);
			}
			else {
				if (curl_thread->joinable()) curl_thread->join();
				curl_easy_cleanup(curl);
				curl_slist_free_all(headers_list);

				res.set_content(ctx->full_body_buffer, ctx->content_type);
			}

			success = true;
			break;
		}

		if (!success) {
			res.status = 502;
			res.set_content("All connection attempts failed.", "text/plain");
		}
		};

	// Generic fallback wildcard routing
	auto generic_handler = [&](const httplib::Request& req, httplib::Response& res) {
		std::string path = req.path;
		if (path.rfind("/v1/", 0) == 0) {
			path = path.substr(4);
		}
		handle_proxy_v1(path, req, res);
		};

	svr.Get(R"(/v1/(.*))", generic_handler);
	svr.Post(R"(/v1/(.*))", generic_handler);
	svr.Put(R"(/v1/(.*))", generic_handler);
	svr.Delete(R"(/v1/(.*))", generic_handler);
	svr.Options(R"(/v1/(.*))", generic_handler);
	svr.Patch(R"(/v1/(.*))", generic_handler);

	std::cout << "Starting cross-platform NIM Load Balancer on http://127.0.0.1:8101\n";
	svr.listen("127.0.0.1", 8101);

	shutdown.store(true);
	if (sync_thread.joinable()) sync_thread.join();
	curl_global_cleanup();
	return 0;
}

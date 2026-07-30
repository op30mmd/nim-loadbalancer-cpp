#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include "logger.h"
#include "proxy_config.h"

// ============================================================================
// Key & Backoff Management
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
				std::string masked = (key.size() > 6) ? key.substr(key.size() - 6) : key;
				LOG_WARN("KeyManager", "Key ..." + masked + " flagged on HTTP " + std::to_string(status_code) + ". Cooldown: " + std::to_string(duration) + "s");
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
	ClientSideBackoff(int max_concurrent = 2, double interval = 1.71)  // ~35 RPM safe margin
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
			LOG_WARN("Backoff", "Global backoff active. Throttling request for " + std::to_string(wait_dur.count() / 1000.0) + "s");
			std::this_thread::sleep_for(wait_dur);
			now = std::chrono::steady_clock::now();
		}

		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time).count() / 1000.0;
		if (elapsed < min_interval) {
			double wait_sec = min_interval - elapsed;
			LOG_DEBUG("Backoff", "Sequential request throttled. Waiting " + std::to_string(wait_sec) + "s");
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
			LOG_WARN("Backoff", "Global backoff triggered for " + std::to_string(duration) + "s");
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

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

// Classification of an upstream failure. It drives how aggressively the
// offending key is backed off: a key that looks invalid (401/403) gets a long
// rest, a throttled key (429) gets a medium one, and a transient upstream hiccup
// (5xx) gets a short one.
enum class KeyErrorClass {
	AUTH,        // 401 / 403 - key likely invalid, revoked, or unauthorized
	RATE_LIMIT,  // 429       - upstream is throttling this key
	SERVER,      // 5xx       - transient upstream error, not the key's fault
	NETWORK,     // curl-level / init failures - transient, retry-friendly
	UNKNOWN      // anything else
};

struct KeyItem {
	std::string key;
	std::chrono::steady_clock::time_point cooldown_until{};

	// --- Smart rotation health tracking ---
	int consecutive_failures = 0;   // resets to 0 on success; drives exponential backoff
	int total_requests = 0;
	int total_successes = 0;
	int total_failures = 0;
	std::chrono::steady_clock::time_point last_used{};
	std::chrono::steady_clock::time_point last_failed{};
};

// Read-only view of a key's runtime state, used for logging and the /v1/keys
// status endpoint. The key itself is masked so it is safe to expose.
struct KeySnapshot {
	std::string masked;
	std::string state;   // "available" or "cooldown"
	int consecutive_failures = 0;
	int total_requests = 0;
	int total_successes = 0;
	int total_failures = 0;
	long long cooldown_remaining_sec = 0;
};

// Round-robin key rotation with adaptive, error-aware backoff.
//
// Selection is plain round-robin: a cursor advances across the key pool and the
// first key past its cooldown is handed out, so load spreads evenly. The
// "smarts" live in the health layer on top of that cursor:
//   * each failure escalates that key's cooldown exponentially (capped);
//   * the base cooldown depends on the failure class (auth / rate-limit / server);
//   * a success fully resets the key's failure streak and cooldown, so a key
//     that recovers is immediately trusted again instead of staying penalized.
class KeyManager {
private:
	std::vector<KeyItem> keys;
	int base_cooldown;   // seconds - baseline cooldown for the rate-limit class
	int max_cooldown;    // seconds - hard cap for exponential backoff
	size_t index = 0;    // round-robin cursor
	std::mutex mtx;

	static std::string mask(const std::string& key) {
		return (key.size() > 6) ? ("..." + key.substr(key.size() - 6)) : key;
	}

	static KeyErrorClass classify(int status_code) {
		if (status_code == 401 || status_code == 403) return KeyErrorClass::AUTH;
		if (status_code == 429) return KeyErrorClass::RATE_LIMIT;
		if (status_code >= 500 && status_code < 600) return KeyErrorClass::SERVER;
		if (status_code < 0) return KeyErrorClass::NETWORK;
		return KeyErrorClass::UNKNOWN;
	}

	static const char* class_name(KeyErrorClass cls) {
		switch (cls) {
		case KeyErrorClass::AUTH: return "auth";
		case KeyErrorClass::RATE_LIMIT: return "rate-limit";
		case KeyErrorClass::SERVER: return "server";
		case KeyErrorClass::NETWORK: return "network";
		default: return "unknown";
		}
	}

	// Exponential backoff: base * 2^(consecutive_failures - 1), capped at
	// max_cooldown. The exponent is clamped to avoid overflow.
	int compute_cooldown(KeyErrorClass cls, int consecutive_failures) const {
		int base;
		switch (cls) {
		case KeyErrorClass::AUTH:
			// A key that fails auth is probably bad - give it a long rest,
			// but still let it recover on a later success.
			base = std::max(base_cooldown, 300);
			break;
		case KeyErrorClass::SERVER:
			// Transient upstream blip - don't punish the key for long.
			base = std::max(5, base_cooldown / 4);
			break;
		case KeyErrorClass::RATE_LIMIT:
		case KeyErrorClass::NETWORK:
		case KeyErrorClass::UNKNOWN:
		default:
			base = base_cooldown;
			break;
		}

		int exp = consecutive_failures - 1;
		if (exp < 0) exp = 0;
		if (exp > 7) exp = 7;  // cap at 128x to avoid overflow

		int duration = base << exp;
		if (duration > max_cooldown) duration = max_cooldown;
		if (duration < 1) duration = 1;
		return duration;
	}

public:
	KeyManager(const std::vector<std::string>& raw_keys, int cooldown = 60, int max_cooldown = 1800)
		: base_cooldown(cooldown), max_cooldown(max_cooldown) {
		for (const auto& k : raw_keys) {
			KeyItem item;
			item.key = k;
			keys.push_back(std::move(item));
		}
	}

	// Returns the next key in round-robin order, skipping any that are still in
	// cooldown. If every key is cooling down, degrades gracefully by returning
	// the key whose cooldown expires soonest (instead of a hard empty/503), so a
	// brief cooldown overlap doesn't fail the request. Returns "" only when no
	// keys are configured at all.
	std::string get_key() {
		std::lock_guard<std::mutex> lock(mtx);
		if (keys.empty()) return "";
		auto now = std::chrono::steady_clock::now();
		size_t total = keys.size();

		// Round-robin scan: advance the cursor and return the first available key.
		for (size_t i = 0; i < total; ++i) {
			KeyItem& candidate = keys[index];
			index = (index + 1) % total;
			if (candidate.cooldown_until <= now) {
				candidate.total_requests++;
				candidate.last_used = now;
				return candidate.key;
			}
		}

		// All keys cooling down: hand out the soonest-available one.
		size_t soonest = 0;
		for (size_t i = 1; i < total; ++i) {
			if (keys[i].cooldown_until < keys[soonest].cooldown_until) {
				soonest = i;
			}
		}
		KeyItem& pick = keys[soonest];
		long long remaining = std::chrono::duration_cast<std::chrono::seconds>(
			pick.cooldown_until - now).count();
		LOG_WARN("KeyManager", "All " + std::to_string(total) + " keys in cooldown. "
			"Returning soonest-available " + mask(pick.key)
			+ " (~" + std::to_string(remaining) + "s remaining).");
		pick.total_requests++;
		pick.last_used = now;
		return pick.key;
	}

	void mark_failed(const std::string& key, int status_code) {
		std::lock_guard<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		for (auto& item : keys) {
			if (item.key == key) {
				item.consecutive_failures++;
				item.total_failures++;
				item.last_failed = now;

				KeyErrorClass cls = classify(status_code);
				int duration = compute_cooldown(cls, item.consecutive_failures);
				item.cooldown_until = now + std::chrono::seconds(duration);

				LOG_WARN("KeyManager", "Key " + mask(key) + " flagged on HTTP "
					+ std::to_string(status_code) + " [" + class_name(cls) + "]. "
					"Backoff: " + std::to_string(duration) + "s "
					"(fail #" + std::to_string(item.consecutive_failures) + ")");
				break;
			}
		}
	}

	// Reports a successful use of `key`. Clears its consecutive-failure streak and
	// any active cooldown so a recovering key is trusted again immediately.
	void mark_success(const std::string& key) {
		std::lock_guard<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		for (auto& item : keys) {
			if (item.key == key) {
				item.total_successes++;
				item.last_used = now;
				if (item.consecutive_failures > 0) {
					LOG_INFO("KeyManager", "Key " + mask(key) + " recovered after "
						+ std::to_string(item.consecutive_failures)
						+ " consecutive failure(s).");
				}
				item.consecutive_failures = 0;
				item.cooldown_until = std::chrono::steady_clock::time_point();
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

	// Point-in-time, masked view of every key's health for logging/endpoints.
	std::vector<KeySnapshot> snapshot() {
		std::lock_guard<std::mutex> lock(mtx);
		auto now = std::chrono::steady_clock::now();
		std::vector<KeySnapshot> out;
		out.reserve(keys.size());
		for (auto& item : keys) {
			KeySnapshot s;
			s.masked = mask(item.key);
			s.consecutive_failures = item.consecutive_failures;
			s.total_requests = item.total_requests;
			s.total_successes = item.total_successes;
			s.total_failures = item.total_failures;
			bool available = (item.cooldown_until <= now);
			s.state = available ? "available" : "cooldown";
			s.cooldown_remaining_sec = available ? 0
				: std::chrono::duration_cast<std::chrono::seconds>(item.cooldown_until - now).count();
			out.push_back(std::move(s));
		}
		return out;
	}

	// Compact one-line summary for periodic logging.
	std::string status_summary() {
		auto snap = snapshot();
		std::string s = std::to_string(snap.size()) + " keys | ";
		for (size_t i = 0; i < snap.size(); ++i) {
			if (i) s += ", ";
			s += snap[i].masked + ":" + snap[i].state
				+ "(" + std::to_string(snap[i].consecutive_failures) + "f/"
				+ std::to_string(snap[i].total_requests) + "u)";
		}
		return s;
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

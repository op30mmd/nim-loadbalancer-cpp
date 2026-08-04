#define _CRT_SECURE_NO_WARNINGS

// ======================================================================
//  THIS PROJECT **REQUIRES** C++17
// ======================================================================
// Structured bindings:   auto [prov, key, url] = ...
// Generic lambdas, etc.
//
// IN VISUAL STUDIO (do this after every pull):
//   1. Right-click the project in Solution Explorer
//   2. Properties → Configuration: All Configurations | Platform: All Platforms
//   3. C/C++ → Language → C++ Language Standard
//      →  "ISO C++17 Standard (/std:c++17)"   or "C++17 (/std:c++17)"
//   4. Apply → OK
//   5. Build → Clean Solution
//   6. Build → Rebuild Solution
//
// If you still see "expected a ']'" or "cannot deduce 'auto' type",
// the compiler is NOT using C++17.
// ======================================================================

// Compile-time guard (will produce a clear error if not C++17)
#if __cplusplus < 201703L && !defined(_MSVC_LANG)
#error "This project requires C++17. Set /std:c++17 (or ISO C++17) in your compiler settings."
#elif defined(_MSVC_LANG) && _MSVC_LANG < 201703L
#error "This project requires C++17. Set C++ Language Standard to ISO C++17 Standard (/std:c++17) in Visual Studio project properties."
#endif

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
#include <functional>
#include <cstring>
#include <tuple>
#include <utility>
#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <ctime>

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
#include "stats_collector.h"
#include "tui_panel.h"
#include "provider_manager.h"
#include "proxy_handlers.h"

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
	ClientSideBackoff backoff_manager(2, 1.71);
	ModelCache model_cache(3600);

	ProviderManager provider_manager;
	provider_manager.initialize_default(keys, key_cooldown, key_max_cooldown);

	LOG_INFO("Startup", "Key rotation: round-robin with adaptive backoff | keys=" + std::to_string(keys.size())
		+ " | base=" + std::to_string(key_cooldown) + "s"
		+ " | cap=" + std::to_string(key_max_cooldown) + "s");

	std::atomic<bool> shutdown(false);

	// --- Statistics & TUI ---
	StatsCollector stats;
	ProxyHandlers handlers(key_manager, backoff_manager, model_cache, provider_manager, stats);
	TUIPanel tui(key_manager, stats, shutdown, &provider_manager, key_cooldown, key_max_cooldown);
	g_logger.set_tui_mode(true, [&tui](const std::string& line) { tui.push_log(line); });

	// Only start the TUI if stdout is a real console (not a pipe/redirect/service)
#ifdef _WIN32
	HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode_check;
	bool is_console = GetConsoleMode(h_out, &mode_check) != 0;
#else
	bool is_console = isatty(fileno(stdout)) != 0;
#endif

	httplib::Server svr;
	tui.set_stop_callback([&svr, &handlers]() {
		handlers.stop_streaming();
		svr.stop();
	});
	if (is_console) tui.start();

	svr.set_read_timeout(600, 0);
	svr.set_write_timeout(30, 0);

	// All routes (CORS middleware, health, keys, models, messages, wildcard
	// OpenAI proxy, providers) live in ProxyHandlers — see proxy_handlers.cpp.
	handlers.setup_routes(svr);

	LOG_INFO("Server", "NVIDIA NIM Proxy listening on http://127.0.0.1:8100");
	LOG_INFO("Server", " - Health Endpoint:    http://127.0.0.1:8100/health");
	LOG_INFO("Server", " - Keys Endpoint:      http://127.0.0.1:8100/v1/keys");
	LOG_INFO("Server", " - Models Endpoint:    http://127.0.0.1:8100/v1/models");
	LOG_INFO("Server", " - OpenAI Endpoint:    http://127.0.0.1:8100/v1/chat/completions");
	LOG_INFO("Server", " - Anthropic Endpoint: http://127.0.0.1:8100/v1/messages");

	svr.listen("127.0.0.1", 8100);

	shutdown.store(true);
	if (is_console) tui.stop();
	g_logger.set_tui_mode(false);
	curl_global_cleanup();
	return 0;
}

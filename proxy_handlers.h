#pragma once

#include <atomic>
#include <string>
#include <tuple>
#include <utility>

#include <curl/curl.h>
#include <httplib.h>

#include "key_manager.h"
#include "logger.h"
#include "provider_manager.h"
#include "proxy_config.h"
#include "stats_collector.h"

// ============================================================================
// HTTP Request Handlers
// ============================================================================
//
// All route handlers for the proxy server live here (extracted from main.cpp)
// so they can be unit/integration tested in-process. The class owns no state
// beyond references to the shared components (key rotation, backoff, model
// cache, provider routing, statistics) and a shutdown flag that aborts
// in-flight streaming content providers.
class ProxyHandlers {
public:
	ProxyHandlers(KeyManager& key_manager, ClientSideBackoff& backoff_manager,
		ModelCache& model_cache, ProviderManager& provider_manager,
		StatsCollector& stats);

	// Registers the CORS pre-routing middleware and every HTTP route on svr.
	void setup_routes(httplib::Server& svr);

	// Signals in-flight streaming content providers to stop promptly (used on
	// shutdown so chunk loops exit instead of waiting out the write timeout).
	void stop_streaming() { streaming_shutdown.store(true); }

	// Individual handlers (public so tests can drive them directly if needed).
	void handle_health(const httplib::Request& req, httplib::Response& res);
	void handle_keys(const httplib::Request& req, httplib::Response& res);
	void handle_models(const httplib::Request& req, httplib::Response& res);
	void handle_anthropic_messages(const httplib::Request& req, httplib::Response& res);
	void handle_openai_proxy(const httplib::Request& req, httplib::Response& res);
	void handle_providers(const httplib::Request& req, httplib::Response& res);

private:
	// Prefers configured providers, falls back to the legacy KeyManager pool.
	std::tuple<BackendProvider*, std::string, std::string> get_next_backend();

	KeyManager& key_manager;
	ClientSideBackoff& backoff_manager;
	ModelCache& model_cache;
	ProviderManager& provider_manager;
	StatsCollector& stats;

	std::atomic<bool> streaming_shutdown{ false };
};

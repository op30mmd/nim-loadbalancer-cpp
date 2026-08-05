// Integration tests for ProxyHandlers: health, CORS middleware, status
// endpoints and the /v1/models catalog. Split from the OpenAI/Anthropic
// handler tests (test_handlers_b.cpp) to keep TUs small under --coverage.
#include <catch2/catch_all.hpp>

#include <memory>
#include <string>

#include "test_handlers_fixtures.h"

// ---------------------------------------------------------------------------
// Health / middleware / status endpoints
// ---------------------------------------------------------------------------

TEST_CASE("health endpoint returns ok", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/health");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->body.find("\"ok\"") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("OPTIONS preflight returns 204 with CORS headers", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Options("/v1/chat/completions");
	REQUIRE(res);
	REQUIRE(res->status == 204);
	REQUIRE(res->get_header_value("Access-Control-Allow-Origin") == "*");
	REQUIRE(res->get_header_value("Access-Control-Allow-Methods").find("POST") != std::string::npos);
	REQUIRE_FALSE(res->get_header_value("x-request-id").empty());

	// Regular requests also carry CORS + request id headers
	auto get = proxy.client()->Get("/health");
	REQUIRE(get->get_header_value("Access-Control-Allow-Origin") == "*");
	REQUIRE_FALSE(get->get_header_value("x-request-id").empty());
	proxy.stop();
	upstream.stop();
}

TEST_CASE("keys endpoint lists masked key health", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/v1/keys");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["strategy"] == "round-robin");
	REQUIRE(j["keys"].size() == 2);
	REQUIRE(j["keys"][0]["state"] == "available");
	REQUIRE(j["keys"][0]["key"].get<std::string>().find("...") != std::string::npos);
	REQUIRE(j["keys"][0]["total_requests"] == 0);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("providers endpoint lists configured providers", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/v1/providers");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["providers"].size() == 1);
	REQUIRE(j["providers"][0]["name"] == "NVIDIA NIM");
	REQUIRE(j["providers"][0]["enabled"] == true);
	REQUIRE(j["providers"][0]["key_count"] == 2);
	proxy.stop();
	upstream.stop();
}

// ---------------------------------------------------------------------------
// /v1/models catalog
// ---------------------------------------------------------------------------

TEST_CASE("models endpoint fetches and caches the catalog", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto cli = proxy.client();
	auto first = cli->Get("/v1/models");
	REQUIRE(first);
	REQUIRE(first->status == 200);
	REQUIRE(first->body.find("meta/llama-3.1-8b-instruct") != std::string::npos);
	REQUIRE(upstream.models_hits.load() == 1);

	// Second request (and the /models alias) served from cache
	auto second = cli->Get("/v1/models");
	REQUIRE(second->status == 200);
	auto alias = cli->Get("/models");
	REQUIRE(alias->status == 200);
	REQUIRE(upstream.models_hits.load() == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("models endpoint forwards upstream errors and flags the key", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.models_status = 403;
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 403);

	// The provider (and its key) that hit the 403 is now in error state
	auto snap = proxy.providers->snapshot();
	REQUIRE(snap[0].status == "error");
	auto active = proxy.providers->get_active_providers();
	REQUIRE(active[0]->key_manager->snapshot()[0].state == "cooldown");
	REQUIRE(active[0]->key_manager->snapshot()[0].consecutive_failures == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("models endpoint returns 502 on upstream network failure", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to("http://127.0.0.1:1/v1");  // nothing listens on port 1
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("models endpoint returns 503 without keys or providers", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy(false, false);  // no keys, no providers
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 503);
	proxy.stop();
	upstream.stop();
}

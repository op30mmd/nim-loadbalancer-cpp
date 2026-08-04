// Integration tests for the remaining ProxyHandlers paths: legacy key-pool
// fallback (via the base-URL override seam), error-body extraction, thinking
// scaling, alt routes, and stream end-of-buffer flushing.
#include <catch2/catch_all.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "test_handlers_fixtures.h"

// ---------------------------------------------------------------------------
// Legacy fallback (no providers configured -> round-robin key pool)
// ---------------------------------------------------------------------------

// RAII: redirects the legacy fallback URL to a mock upstream for the test.
struct LegacyUrlGuard {
	explicit LegacyUrlGuard(const std::string& url) {
		set_nvidia_base_url_override(url);
	}
	~LegacyUrlGuard() {
		set_nvidia_base_url_override("");
	}
};

TEST_CASE("legacy fallback serves the models catalog", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);  // no providers, keys only
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->body.find("meta/llama-3.1-8b-instruct") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback marks the key on models auth errors", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	upstream.models_status = 403;
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 403);
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");
	REQUIRE(proxy.km->snapshot()[0].total_failures == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback routes openai requests with the pool key", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->body == upstream.chat_body);
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		REQUIRE(upstream.last_auth_header == "Bearer nvapi-test-key-1");
	}
	REQUIRE(proxy.km->snapshot()[0].total_successes == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback marks the pool key on openai 429", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 429;
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 429);
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");
	REQUIRE(proxy.km->snapshot()[0].consecutive_failures == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback returns 502 on network failure", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard("http://127.0.0.1:1/v1");  // nothing listens here
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");  // marked via -1
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback marks the key on models network failure", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard("http://127.0.0.1:1/v1");
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback marks the key on anthropic network failure", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard("http://127.0.0.1:1/v1");
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback routes anthropic messages", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "message");
	REQUIRE(j["content"][0]["text"] == "Mock reply");
	REQUIRE(proxy.km->snapshot()[0].total_successes == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("legacy fallback marks the pool key on anthropic 429", "[handlers][legacy]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 429;
	LegacyUrlGuard url_guard(upstream.base_url());
	TestProxy proxy(false, true);
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 429);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["type"] == "rate_limit_error");
	REQUIRE(proxy.km->snapshot()[0].state == "cooldown");
	proxy.stop();
	upstream.stop();
}

// ---------------------------------------------------------------------------
// Provider edge cases
// ---------------------------------------------------------------------------

TEST_CASE("providers with no usable keys fall back to 503", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy(false, false);
	// Empty-string key pools: load_provider_keys returns the fallback as-is,
	// so every provider's pool is unusable. Drop the dummy-keyed non-NVIDIA
	// providers (they would be routed to real upstreams); NVIDIA keeps its
	// empty-string pool and routing 503s without ever touching the network.
	proxy.providers->initialize_default({ "", "" }, 60, 1800);
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 503);
	proxy.stop();
	upstream.stop();
}

// ---------------------------------------------------------------------------
// Alt routes, methods and error bodies
// ---------------------------------------------------------------------------

TEST_CASE("POST /messages is served by the anthropic handler", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(upstream.chat_hits == 1);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "message");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages returns 503 without keys or providers", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy(false, false);
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 503);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "error");
	REQUIRE(j["error"]["type"] == "overloaded_error");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages returns 502 on upstream network failure", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to("http://127.0.0.1:1/v1");
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["type"] == "overloaded_error");
	REQUIRE(proxy.providers->snapshot()[0].status == "cooldown");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages extracts string error bodies", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 400;
	upstream.chat_body = R"({"error":"bad request"})";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["message"] == "bad request");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages extracts top-level message errors", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 400;
	upstream.chat_body = R"({"message":"top level failure"})";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["message"] == "top level failure");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages falls back to raw body for non-JSON errors", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 400;
	upstream.chat_body = "plain failure text";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["message"] == "plain failure text");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages returns 500 when the upstream body cannot be converted", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_body = "this is not json";  // status stays 200
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 500);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["type"] == "api_error");
	REQUIRE(j["error"]["message"] == "Failed to transform response");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages scales max_tokens for thinking budgets", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"thinking":{"type":"enabled","budget_tokens":1000},"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		auto body = nlohmann::json::parse(upstream.last_request_body);
		// 64 + 1000 capped at 8192
		REQUIRE(body["max_tokens"] == 1064);
	}
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages defaults thinking budget when unset", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"thinking":{"type":"enabled"},"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		auto body = nlohmann::json::parse(upstream.last_request_body);
		// max(64*4, 4096) capped at 8192
		REQUIRE(body["max_tokens"] == 4096);
	}
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages clamps max_tokens to the context window", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":100000,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		auto body = nlohmann::json::parse(upstream.last_request_body);
		// Unknown model -> 32768 context; clamped below that.
		REQUIRE(body["max_tokens"] < 100000);
		REQUIRE(body["max_tokens"] <= 32768);
	}
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages flushes partial lines when the stream ends", "[handlers]") {
	// Upstream sends the [DONE] marker without a trailing newline so the final
	// line sits in the proxy's buffer when the queue finishes.
	httplib::Server upstream;
	upstream.Post("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
		auto done = std::make_shared<bool>(false);
		res.set_chunked_content_provider("text/event-stream",
			[done](size_t, httplib::DataSink& sink) {
				if (*done) return false;
				*done = true;
				const char* events =
					"data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
					"data: [DONE]";  // no trailing newline
				sink.write(events, std::strlen(events));
				sink.done();
				return false;
			});
	});
	int port = upstream.bind_to_any_port("127.0.0.1");
	std::thread upstream_thread([&]() { upstream.listen_after_bind(); });

	TestProxy proxy;
	proxy.route_to("http://127.0.0.1:" + std::to_string(port) + "/v1");
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":true,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->body.find("event: message_start") != std::string::npos);
	REQUIRE(res->body.find("event: message_stop") != std::string::npos);
	proxy.stop();
	upstream.stop();
	upstream_thread.join();
}

TEST_CASE("openai passthrough supports GET requests", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/v1/chat/completions");
	REQUIRE(res);
	// The mock only registers POST /v1/chat/completions -> 404 from upstream.
	REQUIRE(res->status == 404);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough supports custom HTTP methods", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Put("/v1/chat/completions", "{}", "application/json");
	REQUIRE(res);
	REQUIRE(res->status == 404);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("models endpoint forwards generic upstream errors", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.models_status = 400;
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/v1/models");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	REQUIRE(res->body.find("Failed fetching models") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

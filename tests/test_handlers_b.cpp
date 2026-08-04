// Integration tests: OpenAI passthrough + Anthropic messages handlers.
// Split from test_handlers.cpp into its own TU to keep translation units
// small enough for the MinGW assembler under --coverage -O0.
#include <catch2/catch_all.hpp>

#include <memory>
#include <string>

#include "test_handlers_fixtures.h"

// ---------------------------------------------------------------------------
// OpenAI passthrough
// ---------------------------------------------------------------------------

TEST_CASE("openai passthrough forwards requests and responses", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->body == upstream.chat_body);
	REQUIRE(upstream.chat_hits == 1);
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		REQUIRE(upstream.last_auth_header == "Bearer nvapi-test-key-1");
	}
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough forwards 429 and marks the provider key", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 429;
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 429);
	REQUIRE(res->body == upstream.chat_body);

	// Provider health reflects the cooldown; the used key is cooling down.
	auto snap = proxy.providers->snapshot();
	REQUIRE(snap[0].status == "cooldown");
	auto active = proxy.providers->get_active_providers();
	REQUIRE(active[0]->key_manager->snapshot()[0].state == "cooldown");
	REQUIRE(active[0]->key_manager->snapshot()[0].consecutive_failures == 1);
	REQUIRE(active[0]->key_manager->snapshot()[0].total_failures == 1);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough forwards 401 and marks the key", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 401;
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 401);
	REQUIRE(res->body == upstream.chat_body);
	REQUIRE(proxy.providers->snapshot()[0].status == "error");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough forwards non-flagged errors without marking the key", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 400;
	upstream.chat_body = R"({"error":{"message":"invalid prompt"}})";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	REQUIRE(res->body.find("invalid prompt") != std::string::npos);

	// 400 is not a key-flagging status: provider stays ready, key untouched.
	REQUIRE(proxy.providers->snapshot()[0].status == "ready");
	auto active = proxy.providers->get_active_providers();
	REQUIRE(active[0]->key_manager->snapshot()[0].state == "available");
	REQUIRE(active[0]->key_manager->snapshot()[0].total_failures == 0);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough returns 502 on upstream network failure", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	// Nothing listens on port 1: connect is refused instantly.
	TestProxy proxy;
	proxy.route_to("http://127.0.0.1:1/v1");
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 502);
	REQUIRE(res->body.find("Upstream network error") != std::string::npos);
	REQUIRE(proxy.providers->snapshot()[0].status == "cooldown");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai passthrough streams SSE events verbatim", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","stream":true,"messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->get_header_value("Content-Type") == "text/event-stream");
	REQUIRE(res->body.find("Hello") != std::string::npos);
	REQUIRE(res->body.find(" world") != std::string::npos);
	REQUIRE(res->body.find("[DONE]") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("wildcard route forwards /v1/embeddings", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/embeddings",
		R"({"model":"nvidia/embed-qa-4","input":"hello"})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(upstream.embeddings_hits == 1);
	REQUIRE(res->body.find("\"object\":\"list\"") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("openai endpoints return 503 without keys or providers", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy(false, false);
	proxy.start();

	auto res = proxy.client()->Post("/v1/chat/completions",
		R"({"model":"meta/llama-3.1-8b-instruct","messages":[{"role":"user","content":"Hi"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 503);
	REQUIRE(res->body.find("No providers/keys available") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

// ---------------------------------------------------------------------------
// Anthropic messages
// ---------------------------------------------------------------------------

TEST_CASE("anthropic messages endpoint converts requests and responses", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(upstream.chat_hits == 1);

	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "message");
	REQUIRE(j["role"] == "assistant");
	REQUIRE(j["model"] == "custom-claude-x");
	REQUIRE(j["content"][0]["type"] == "text");
	REQUIRE(j["content"][0]["text"] == "Mock reply");
	REQUIRE(j["usage"]["output_tokens"] == 2);
	REQUIRE(j["usage"]["input_tokens"] >= 4);

	// The upstream request was converted to OpenAI shape with the mapped model.
	{
		std::lock_guard<std::mutex> lock(upstream.auth_mtx);
		REQUIRE(upstream.last_auth_header == "Bearer nvapi-test-key-1");
		auto body = nlohmann::json::parse(upstream.last_request_body);
		REQUIRE(body["model"] == "custom/nim-model");
		REQUIRE(body["max_tokens"] == 64);
		REQUIRE(body["messages"][0]["role"] == "user");
	}
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages returns rate_limit_error on 429 and marks the key", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 429;
	upstream.chat_body = R"({"error":{"message":"rate limited"}})";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 429);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "error");
	REQUIRE(j["error"]["type"] == "rate_limit_error");
	REQUIRE(proxy.providers->snapshot()[0].status == "cooldown");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages extracts upstream error messages", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	upstream.chat_status = 400;
	upstream.chat_body = R"({"error":{"message":"bad model"}})";
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "error");
	REQUIRE(j["error"]["type"] == "invalid_request_error");
	REQUIRE(j["error"]["message"] == "bad model");
	// 400 is not a key-flagging status
	REQUIRE(proxy.providers->snapshot()[0].status == "ready");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages endpoint converts streaming events", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":64,"stream":true,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(res->get_header_value("Content-Type") == "text/event-stream");
	REQUIRE(res->body.find("event: message_start") != std::string::npos);
	REQUIRE(res->body.find("event: content_block_start") != std::string::npos);
	REQUIRE(res->body.find("event: content_block_delta") != std::string::npos);
	REQUIRE(res->body.find("Hello") != std::string::npos);
	REQUIRE(res->body.find(" world") != std::string::npos);
	REQUIRE(res->body.find("event: message_delta") != std::string::npos);
	REQUIRE(res->body.find("event: message_stop") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages returns empty response for max_tokens=0 pre-warm", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","max_tokens":0,"stream":false,"messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE(upstream.chat_hits == 0);  // never reaches the upstream
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "message");
	REQUIRE(j["content"].empty());
	REQUIRE(j["stop_reason"] == "end_turn");
	REQUIRE(j["usage"]["input_tokens"] == 0);
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages rejects invalid JSON", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages", "not json at all", "application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["type"] == "error");
	REQUIRE(j["error"]["type"] == "invalid_request_error");
	REQUIRE(j["error"]["message"] == "Invalid JSON");
	proxy.stop();
	upstream.stop();
}

TEST_CASE("anthropic messages requires max_tokens", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Post("/v1/messages",
		R"({"model":"custom-claude-x","messages":[{"role":"user","content":"Hello"}]})",
		"application/json");
	REQUIRE(res);
	REQUIRE(res->status == 400);
	auto j = nlohmann::json::parse(res->body);
	REQUIRE(j["error"]["type"] == "invalid_request_error");
	REQUIRE(j["error"]["message"].get<std::string>().find("max_tokens") != std::string::npos);
	proxy.stop();
	upstream.stop();
}

// ---------------------------------------------------------------------------
// Misc middleware
// ---------------------------------------------------------------------------

TEST_CASE("responses carry a generated x-request-id header", "[handlers]") {
	MockUpstream upstream;
	upstream.start();
	TestProxy proxy;
	proxy.route_to(upstream.base_url());
	proxy.start();

	auto res = proxy.client()->Get("/health");
	REQUIRE(res);
	REQUIRE(res->status == 200);
	REQUIRE_FALSE(res->get_header_value("x-request-id").empty());
	proxy.stop();
	upstream.stop();
}

#include <catch2/catch_all.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "test_helpers.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Creates (or removes) a file in the current working directory.
static void write_file(const std::string& path, const std::string& content) {
	std::ofstream f(path);
	f << content;
}

static void remove_file(const std::string& path) {
	std::remove(path.c_str());
}

// NIM_MODEL_MAP must be present before the first call to
// map_anthropic_model_to_nim (it caches the env value in a static). A global
// constructor runs before main(), so this is deterministic. The override map
// uses a key that does NOT collide with the built-in mappings.
namespace {
struct ModelMapEnvSetter {
	ModelMapEnvSetter() {
		nim_set_env("NIM_MODEL_MAP", "{\"custom-claude-x\":\"custom/nim-model\"}");
		// Hermetic tests: never route local 127.0.0.1 traffic through an
		// ambient HTTP proxy (libcurl honors these env vars).
		nim_set_env("HTTP_PROXY", nullptr);
		nim_set_env("HTTPS_PROXY", nullptr);
		nim_set_env("ALL_PROXY", nullptr);
		nim_set_env("NO_PROXY", nullptr);
		nim_set_env("http_proxy", nullptr);
		nim_set_env("https_proxy", nullptr);
		nim_set_env("all_proxy", nullptr);
		nim_set_env("no_proxy", nullptr);
	}
};
ModelMapEnvSetter model_map_env_setter;
}

// ---------------------------------------------------------------------------
// file_exists / get_home_dir
// ---------------------------------------------------------------------------

TEST_CASE("file_exists detects existing and missing files", "[utils]") {
	const char* tmp = "nim-test-exists.tmp";
	write_file(tmp, "hello");
	REQUIRE(file_exists(tmp));
	remove_file(tmp);
	REQUIRE_FALSE(file_exists(tmp));
	REQUIRE_FALSE(file_exists("nim-test-never-existed-12345.tmp"));
}

TEST_CASE("get_home_dir returns the platform home directory", "[utils]") {
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
	if (home && *home) {
		REQUIRE(get_home_dir() == std::string(home));
	}
#else
	const char* home = std::getenv("HOME");
	if (home && *home) {
		REQUIRE(get_home_dir() == std::string(home));
	}
	// Unset HOME -> empty result, no crash
	{
		EnvGuard guard("HOME", nullptr);
		REQUIRE(get_home_dir().empty());
	}
#endif
}

// ---------------------------------------------------------------------------
// estimate_input_tokens
// ---------------------------------------------------------------------------

TEST_CASE("estimate_input_tokens counts string message content", "[utils]") {
	nlohmann::json j = {
		{"messages", {{{"role", "user"}, {"content", "abcdefgh"}}}}
	};
	// 8 chars -> (8 + 3) / 4 = 2
	REQUIRE(estimate_input_tokens(j) == 2);
}

TEST_CASE("estimate_input_tokens counts system string + messages", "[utils]") {
	nlohmann::json j = {
		{"system", "0123456789abcdef"},
		{"messages", {{{"role", "user"}, {"content", "1234"}}}}
	};
	// 16 + 4 = 20 -> 5
	REQUIRE(estimate_input_tokens(j) == 5);
}

TEST_CASE("estimate_input_tokens counts system blocks array", "[utils]") {
	nlohmann::json j = {
		{"system", nlohmann::json::array({
			{{"type", "text"}, {"text", "1234"}},
			{{"type", "text"}, {"text", "5678"}},
			{{"type", "image"}, {"source", {{"type", "base64"}, {"data", "zzzz"}}}}
		})},
		{"messages", nlohmann::json::array()}
	};
	// Only text blocks count: 8 chars -> 2
	REQUIRE(estimate_input_tokens(j) == 2);
}

TEST_CASE("estimate_input_tokens counts content blocks in messages", "[utils]") {
	nlohmann::json j = {
		{"messages", nlohmann::json::array({
			{{"role", "user"}, {"content", nlohmann::json::array({
				{{"type", "text"}, {"text", "abcdef"}},
				{{"type", "text"}, {"text", "ghijkl"}}
			})}},
			{{"role", "assistant"}, {"content", "no"}}
		})}
	};
	// 6 + 6 + 2 = 14 -> (14 + 3) / 4 = 4
	REQUIRE(estimate_input_tokens(j) == 4);
}

TEST_CASE("estimate_input_tokens handles empty and malformed inputs", "[utils]") {
	REQUIRE(estimate_input_tokens(nlohmann::json::object()) == 0);
	REQUIRE(estimate_input_tokens(nlohmann::json::array()) == 0);
	REQUIRE(estimate_input_tokens(nlohmann::json({
		{"messages", {{"role", "user"}, {"content", 42}}}
	})) == 0);
}

// ---------------------------------------------------------------------------
// map_anthropic_model_to_nim
// ---------------------------------------------------------------------------

TEST_CASE("map_anthropic_model_to_nim maps built-in Claude models", "[utils]") {
	REQUIRE(map_anthropic_model_to_nim("claude-sonnet-4") == "meta/llama-3.1-405b-instruct");
	REQUIRE(map_anthropic_model_to_nim("claude-3-5-haiku-latest") == "meta/llama-3.1-8b-instruct");
	REQUIRE(map_anthropic_model_to_nim("claude-3-opus-20240229") == "meta/llama-3.1-70b-instruct");
	REQUIRE(map_anthropic_model_to_nim("claude-sonnet-4-5-20250916") == "meta/llama-3.1-405b-instruct");
}

TEST_CASE("map_anthropic_model_to_nim maps built-in OpenAI models", "[utils]") {
	REQUIRE(map_anthropic_model_to_nim("gpt-4o-mini") == "nvidia/nemotron-3-nano-30b-a3b");
	REQUIRE(map_anthropic_model_to_nim("gpt-3.5-turbo") == "nvidia/nemotron-mini-4b-instruct");
}

TEST_CASE("map_anthropic_model_to_nim passes unknown models through", "[utils]") {
	REQUIRE(map_anthropic_model_to_nim("totally-unknown-model-xyz") == "totally-unknown-model-xyz");
}

TEST_CASE("map_anthropic_model_to_nim honors NIM_MODEL_MAP env override", "[utils]") {
	// Set by the global env setter before main(); key does not collide with
	// the built-in map.
	REQUIRE(map_anthropic_model_to_nim("custom-claude-x") == "custom/nim-model");
	// Built-in mappings still resolve when the override has no entry.
	REQUIRE(map_anthropic_model_to_nim("claude-sonnet-4") == "meta/llama-3.1-405b-instruct");
}

// ---------------------------------------------------------------------------
// get_model_context_window
// ---------------------------------------------------------------------------

TEST_CASE("get_model_context_window returns known windows", "[utils]") {
	REQUIRE(get_model_context_window("meta/llama-3.1-405b-instruct") == 131072);
	REQUIRE(get_model_context_window("nvidia/nemotron-mini-4b-instruct") == 4096);
	REQUIRE(get_model_context_window("deepseek/deepseek-r1") == 65536);
	REQUIRE(get_model_context_window("z-ai/glm-5.2") == 131072);
}

TEST_CASE("get_model_context_window falls back to 32768 for unknown models", "[utils]") {
	REQUIRE(get_model_context_window("brand-new/model-9000") == 32768);
}

TEST_CASE("map_anthropic_model_to_nim tolerates an invalid NIM_MODEL_MAP", "[utils]") {
	EnvGuard guard("NIM_MODEL_MAP", "this is not json");
	reset_model_map_env_cache();
	// Restore the valid ambient map even if an assertion below fails
	// (Catch2 runs tests alphabetically and REQUIRE aborts the test case).
	struct CacheRestorer {
		~CacheRestorer() {
			nim_set_env("NIM_MODEL_MAP", "{\"custom-claude-x\":\"custom/nim-model\"}");
			reset_model_map_env_cache();
			map_anthropic_model_to_nim("claude-3-5-sonnet-latest");
		}
	} restorer;
	// Invalid JSON is ignored; the built-in map still applies.
	REQUIRE(map_anthropic_model_to_nim("claude-3-5-sonnet-latest") == "meta/llama-3.1-70b-instruct");
	REQUIRE(map_anthropic_model_to_nim("unknown-model") == "unknown-model");
}

TEST_CASE("map_anthropic_model_to_nim handles an unset NIM_MODEL_MAP", "[utils]") {
	EnvGuard guard("NIM_MODEL_MAP", nullptr);
	reset_model_map_env_cache();
	struct CacheRestorer {
		~CacheRestorer() {
			nim_set_env("NIM_MODEL_MAP", "{\"custom-claude-x\":\"custom/nim-model\"}");
			reset_model_map_env_cache();
			map_anthropic_model_to_nim("claude-3-5-sonnet-latest");
		}
	} restorer;
	REQUIRE(map_anthropic_model_to_nim("claude-3-5-sonnet-latest") == "meta/llama-3.1-70b-instruct");
	REQUIRE(map_anthropic_model_to_nim("claude-sonnet-4") == "meta/llama-3.1-405b-instruct");
}

TEST_CASE("get_home_dir falls back to HOMEDRIVE/HOMEPATH on Windows", "[utils]") {
#ifdef _WIN32
	EnvGuard profile_guard("USERPROFILE", "");
	nim_set_env("USERPROFILE", nullptr);
	EnvGuard drive_guard("HOMEDRIVE", "D:");
	EnvGuard path_guard("HOMEPATH", "\\Users\\tester");
	REQUIRE(get_home_dir() == "D:\\Users\\tester");
#else
	EnvGuard guard("HOME", "/tmp/nim-home");
	REQUIRE(get_home_dir() == "/tmp/nim-home");
#endif
}

// ---------------------------------------------------------------------------
// load_api_keys
// ---------------------------------------------------------------------------

TEST_CASE("load_api_keys parses comma-separated env var with whitespace", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "nvapi-aaa, nvapi-bbb ,\t\nnvapi-ccc");
	remove_file("keys.txt");
	auto keys = load_api_keys();
	REQUIRE(keys.size() == 3);
	REQUIRE(keys[0] == "nvapi-aaa");
	REQUIRE(keys[1] == "nvapi-bbb");
	REQUIRE(keys[2] == "nvapi-ccc");
}

TEST_CASE("load_api_keys skips empty env entries", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "nvapi-aaa,,, nvapi-bbb ,");
	remove_file("keys.txt");
	auto keys = load_api_keys();
	REQUIRE(keys.size() == 2);
}

TEST_CASE("load_api_keys falls back to keys.txt with comments", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "");
	write_file("keys.txt", "# my keys\n\nnvapi-aaa\n  nvapi-bbb  \n# trailing\n");
	auto keys = load_api_keys();
	remove_file("keys.txt");
	REQUIRE(keys.size() == 2);
	REQUIRE(keys[0] == "nvapi-aaa");
	REQUIRE(keys[1] == "nvapi-bbb");
}

TEST_CASE("load_api_keys returns empty when nothing is configured", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "");
	remove_file("keys.txt");
	REQUIRE(load_api_keys().empty());
}

// ---------------------------------------------------------------------------
// load_provider_keys
// ---------------------------------------------------------------------------

TEST_CASE("load_provider_keys nvidia uses fallback master keys", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "");
	auto keys = load_provider_keys("nvidia", {"master-1", "master-2"});
	REQUIRE(keys == std::vector<std::string>({"master-1", "master-2"}));
}

TEST_CASE("load_provider_keys nvidia env wins over fallback", "[utils]") {
	EnvGuard guard("NVIDIA_API_KEY", "nvapi-env");
	auto keys = load_provider_keys("nvidia", {"master-1"});
	REQUIRE(keys == std::vector<std::string>({"nvapi-env"}));
}

TEST_CASE("load_provider_keys falls back to per-provider dummy keys", "[utils]") {
	{
		EnvGuard guard("OPENAI_API_KEY", "");
		REQUIRE(load_provider_keys("openai", {}) == std::vector<std::string>({"sk-openai-dummy"}));
	}
	{
		EnvGuard guard("ANTHROPIC_API_KEY", "");
		REQUIRE(load_provider_keys("anthropic", {}) == std::vector<std::string>({"sk-ant-dummy"}));
	}
	{
		EnvGuard guard("GROQ_API_KEY", "");
		REQUIRE(load_provider_keys("groq", {}) == std::vector<std::string>({"gsk-groq-dummy"}));
	}
	{
		EnvGuard guard("GEMINI_API_KEY", "");
		REQUIRE(load_provider_keys("google", {}) == std::vector<std::string>({"sk-gemini-dummy"}));
	}
	{
		EnvGuard guard("DEEPSEEK_API_KEY", "");
		REQUIRE(load_provider_keys("deepseek", {}) == std::vector<std::string>({"sk-deepseek-dummy"}));
	}
	{
		EnvGuard guard("MISTRAL_API_KEY", "");
		REQUIRE(load_provider_keys("mistral", {}) == std::vector<std::string>({"sk-mistral-dummy"}));
	}
	{
		EnvGuard guard("TOGETHER_API_KEY", "");
		REQUIRE(load_provider_keys("together", {}) == std::vector<std::string>({"sk-together-dummy"}));
	}
	{
		EnvGuard guard("COHERE_API_KEY", "");
		REQUIRE(load_provider_keys("cohere", {}) == std::vector<std::string>({"sk-cohere-dummy"}));
	}
	{
		EnvGuard guard("OPENROUTER_API_KEY", "");
		REQUIRE(load_provider_keys("openrouter", {}) == std::vector<std::string>({"sk-openrouter-dummy"}));
	}
}

TEST_CASE("load_provider_keys parses provider env values", "[utils]") {
	EnvGuard guard("OPENAI_API_KEY", "sk-abc, sk-def ,sk-ghi");
	auto keys = load_provider_keys("openai", {});
	REQUIRE(keys.size() == 3);
	REQUIRE(keys[0] == "sk-abc");
	REQUIRE(keys[1] == "sk-def");
	REQUIRE(keys[2] == "sk-ghi");
}

TEST_CASE("load_provider_keys ollama always returns a key", "[utils]") {
	auto keys = load_provider_keys("ollama", {});
	REQUIRE(keys == std::vector<std::string>({"ollama-no-key-required"}));
}

TEST_CASE("load_provider_keys unknown type yields empty dummy", "[utils]") {
	auto keys = load_provider_keys("mystery-provider", {});
	REQUIRE(keys.size() == 1);
	REQUIRE(keys[0].empty());
}

#ifndef _WIN32
TEST_CASE("load_provider_keys google falls back to GOOGLE_API_KEY", "[utils]") {
	EnvGuard gemini("GEMINI_API_KEY", nullptr);
	EnvGuard google("GOOGLE_API_KEY", "google-key-1");
	auto keys = load_provider_keys("google", {});
	REQUIRE(keys == std::vector<std::string>({"google-key-1"}));
}
#endif

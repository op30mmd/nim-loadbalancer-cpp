#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "provider_manager.h"
#include "test_helpers.h"

namespace {
// All default providers resolve their keys from env vars (with dummy
// fallbacks). Guard the ones that matter so tests are hermetic.
struct ProviderEnvGuard {
	EnvGuard nvidia{ "NVIDIA_API_KEY", "" };
	EnvGuard openai{ "OPENAI_API_KEY", "" };
	EnvGuard anthropic{ "ANTHROPIC_API_KEY", "" };
	EnvGuard gemini{ "GEMINI_API_KEY", "" };
	EnvGuard groq{ "GROQ_API_KEY", "" };
};

static void init_default(ProviderManager& pm) {
	pm.initialize_default({ "master-1", "master-2" }, 60, 1800);
}
}

TEST_CASE("ProviderManager initializes the default provider set", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	pm.initialize_default({ "master-1", "master-2" }, 60, 1800);
	REQUIRE(pm.get_provider_count() == 11);

	auto snap = pm.snapshot();
	REQUIRE(snap.size() == 11);
	REQUIRE(snap[0].name == "NVIDIA NIM");
	REQUIRE(snap[0].type == "nvidia");
	REQUIRE(snap[0].enabled);
	REQUIRE(snap[0].priority == 0);
	REQUIRE(snap[0].status == "ready");
	REQUIRE(snap[0].key_count == 2);
	REQUIRE(snap[0].api_key_masked.find("...") != std::string::npos);

	// Others get a dummy key
	REQUIRE(snap[1].name == "OpenAI");
	REQUIRE(snap[1].key_count == 1);
	REQUIRE(snap[1].base_url == "https://api.openai.com/v1");
	REQUIRE(snap[10].name == "Ollama");
	REQUIRE(snap[10].type == "ollama");
}

TEST_CASE("ProviderManager snapshot of uninitialized manager is empty", "[providers]") {
	ProviderManager pm;
	REQUIRE(pm.snapshot().empty());
	REQUIRE(pm.get_provider_count() == 0);
	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov == nullptr);
	REQUIRE(key.empty());
}

TEST_CASE("get_next_provider_and_key prefers the highest priority provider", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov != nullptr);
	REQUIRE(prov->name == "NVIDIA NIM");
	REQUIRE(key == "master-1");
}

TEST_CASE("get_next_provider_and_key skips disabled providers", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	std::vector<TUIBackendProvider> ui = {
		{"NVIDIA NIM", "nvidia", "", false, 0, "ready"},   // disabled
		{"OpenAI", "openai", "", true, 1, "ready"},
	};
	pm.set_providers(ui);
	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov != nullptr);
	REQUIRE(prov->name == "OpenAI");
	REQUIRE_FALSE(key.empty());
	// NVIDIA still present but disabled
	auto snap = pm.snapshot();
	REQUIRE_FALSE(snap[0].enabled);
}

TEST_CASE("get_next_provider_and_key returns empty when all disabled", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	std::vector<TUIBackendProvider> ui = {
		{"NVIDIA NIM", "nvidia", "", false, 0, "ready"},
	};
	pm.set_providers(ui);
	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov == nullptr);
	REQUIRE(key.empty());
}

TEST_CASE("set_providers updates fields and removes unknown providers", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	std::vector<TUIBackendProvider> ui = {
		{"NVIDIA NIM", "nvidia", "http://localhost:9999/v1", true, 5, "cooldown"},
	};
	pm.set_providers(ui);

	auto snap = pm.snapshot();
	// Everything except NVIDIA NIM was removed
	REQUIRE(snap.size() == 1);
	REQUIRE(snap[0].base_url == "http://localhost:9999/v1");
	REQUIRE(snap[0].priority == 5);
	REQUIRE(snap[0].status == "cooldown");
	// KeyManagers survive the sync
	REQUIRE(snap[0].key_count == 2);
}

TEST_CASE("set_providers adds new providers", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	std::vector<TUIBackendProvider> ui = {
		{"NVIDIA NIM", "nvidia", "", true, 0, "ready"},
		{"Local LLM", "ollama", "http://localhost:11434/v1", true, 9, "ready"},
	};
	pm.set_providers(ui);
	auto snap = pm.snapshot();
	REQUIRE(snap.size() == 2);
	REQUIRE(snap[1].name == "Local LLM");
	REQUIRE(snap[1].base_url == "http://localhost:11434/v1");
	REQUIRE(snap[1].priority == 9);
	REQUIRE(snap[1].key_count == 1);  // ollama always has a key

	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov->name == "NVIDIA NIM");
}

TEST_CASE("mark_provider_failed updates status and key health", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	auto [prov, key] = pm.get_next_provider_and_key();
	REQUIRE(prov != nullptr);

	pm.mark_provider_failed(prov, key, 401);
	REQUIRE(prov->status == "error");
	REQUIRE(prov->key_manager->snapshot()[0].state == "cooldown");
	REQUIRE(prov->key_manager->snapshot()[0].consecutive_failures == 1);

	pm.mark_provider_failed(prov, key, 429);
	REQUIRE(prov->status == "cooldown");

	pm.mark_provider_success(prov, key);
	REQUIRE(prov->status == "ready");
	REQUIRE(prov->key_manager->snapshot()[0].consecutive_failures == 0);
	REQUIRE(prov->key_manager->snapshot()[0].total_successes == 1);
}

TEST_CASE("mark_provider_failed tolerates null providers", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	pm.mark_provider_failed(nullptr, "key", 429);   // no crash
	pm.mark_provider_success(nullptr, "key");       // no crash
}

TEST_CASE("get_active_providers returns priority-sorted enabled providers", "[providers]") {
	ProviderEnvGuard env;
	ProviderManager pm;
	init_default(pm);
	auto active = pm.get_active_providers();
	REQUIRE(active.size() == 11);
	REQUIRE(active[0]->name == "NVIDIA NIM");
	for (size_t i = 1; i < active.size(); ++i) {
		REQUIRE(active[i - 1]->priority <= active[i]->priority);
	}
}

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

#include "key_manager.h"
#include "logger.h"
#include "utils.h"

// This struct MUST be defined before ProviderManager uses it.
struct TUIBackendProvider {
    std::string name;
    std::string type;
    std::string base_url;
    bool enabled = true;
    int priority = 0;
    std::string status = "ready";
    std::string api_key_masked; // informational only
};

struct BackendProvider {
    std::string name;
    std::string type;
    std::string base_url;
    bool enabled = true;
    int priority = 0;
    std::string status = "ready";
    std::unique_ptr<KeyManager> key_manager;
    std::chrono::steady_clock::time_point last_used{};
};

class ProviderManager {
private:
    std::vector<BackendProvider> providers;
    mutable std::mutex mtx;
    std::vector<std::string> master_keys;
    int base_cooldown = 60;
    int max_cooldown = 1800;

public:
    ProviderManager() = default;

    void initialize_default(const std::vector<std::string>& keys,
                            int cooldown = 60,
                            int max_cooldown = 1800) {
        std::lock_guard<std::mutex> lock(mtx);
        master_keys = keys;
        base_cooldown = cooldown;
        this->max_cooldown = max_cooldown;
        providers.clear();

        struct DefaultProviderSpec {
            std::string name;
            std::string type;
            std::string base_url;
            int priority;
        };

        std::vector<DefaultProviderSpec> specs = {
            {"NVIDIA NIM", "nvidia", "https://integrate.api.nvidia.com/v1", 0},
            {"OpenAI", "openai", "https://api.openai.com/v1", 1},
            {"Anthropic", "anthropic", "https://api.anthropic.com/v1", 1},
            {"Google Gemini", "google", "https://generativelanguage.googleapis.com/v1beta", 2},
            {"Groq", "groq", "https://api.groq.com/openai/v1", 2},
            {"DeepSeek", "deepseek", "https://api.deepseek.com", 2},
            {"Mistral AI", "mistral", "https://api.mistral.ai/v1", 3},
            {"Together AI", "together", "https://api.together.xyz/v1", 3},
            {"Cohere", "cohere", "https://api.cohere.com/v1", 3},
            {"OpenRouter", "openrouter", "https://openrouter.ai/api/v1", 3},
            {"Ollama", "ollama", "http://localhost:11434/v1", 4}
        };

        for (const auto& s : specs) {
            BackendProvider p;
            p.name = s.name;
            p.type = s.type;
            p.base_url = s.base_url;
            p.enabled = true;
            p.priority = s.priority;
            p.status = "ready";

            auto p_keys = load_provider_keys(p.type, keys);
            if (!p_keys.empty()) {
                p.key_manager = std::make_unique<KeyManager>(p_keys, cooldown, max_cooldown);
            }
            providers.push_back(std::move(p));
        }
    }

    void set_providers(const std::vector<TUIBackendProvider>& ui_providers,
                       const std::vector<std::string>& fallback_keys = {},
                       int cooldown = 60, int max_cooldown = 1800) {
        std::lock_guard<std::mutex> lock(mtx);

        // Update existing providers in-place (preserves KeyManagers / backoff state).
        // Only structural fields (enabled, priority, base_url, status) are changed;
        // KeyManagers and their cooldown data are never rebuilt on a TUI sync.
        for (auto& bp : providers) {
            for (const auto& up : ui_providers) {
                if (up.name == bp.name) {
                    bp.enabled = up.enabled;
                    bp.priority = up.priority;
                    bp.status = up.status;
                    if (!up.base_url.empty()) bp.base_url = up.base_url;
                    break;
                }
            }
        }

        // Remove any providers that no longer exist in the TUI list (keep only those
        // whose name matches at least one TUI entry, plus NVIDIA NIM which is always
        // kept if present since it's the primary backend).
        providers.erase(
            std::remove_if(providers.begin(), providers.end(), [&](const BackendProvider& bp) {
                for (const auto& up : ui_providers) {
                    if (up.name == bp.name) return false;
                }
                // Keep NVIDIA NIM even if TUI list doesn't mention it
                return bp.name.find("NVIDIA") == std::string::npos;
            }),
            providers.end()
        );

        // Add any new TUI providers that don't already exist in the pool.
        // Only NVIDIA NIM gets keys (it's the only one with real API keys).
        std::vector<std::string> keys_to_use = fallback_keys.empty() ? master_keys : fallback_keys;
        for (const auto& up : ui_providers) {
            if (!up.enabled) continue;
            bool exists = false;
            for (const auto& bp : providers) {
                if (bp.name == up.name) { exists = true; break; }
            }
            if (exists) continue;

            BackendProvider bp;
            bp.name = up.name;
            bp.type = up.type;
            bp.base_url = up.base_url.empty() ? "https://integrate.api.nvidia.com/v1" : up.base_url;
            bp.enabled = true;
            bp.priority = up.priority;
            bp.status = up.status;

            auto p_keys = load_provider_keys(bp.type, keys_to_use);
            if (!p_keys.empty()) {
                bp.key_manager = std::make_unique<KeyManager>(p_keys, cooldown, max_cooldown);
            }
            providers.push_back(std::move(bp));
        }
    }

    // Internal version - caller must hold the lock
    std::vector<BackendProvider*> get_active_providers_unlocked() {
        std::vector<BackendProvider*> active;
        for (auto& p : providers) {
            if (p.enabled && p.key_manager) active.push_back(&p);
        }
        std::sort(active.begin(), active.end(), [](const BackendProvider* a, const BackendProvider* b) {
            return a->priority < b->priority;
        });
        return active;
    }

    std::vector<BackendProvider*> get_active_providers() {
        std::lock_guard<std::mutex> lock(mtx);
        return get_active_providers_unlocked();
    }

    std::pair<BackendProvider*, std::string> get_next_provider_and_key() {
        std::lock_guard<std::mutex> lock(mtx);
        auto active = get_active_providers_unlocked();
        if (active.empty()) return {nullptr, ""};

        for (auto* prov : active) {
            if (prov->key_manager) {
                std::string k = prov->key_manager->get_key();
                if (!k.empty()) {
                    prov->last_used = std::chrono::steady_clock::now();
                    return {prov, k};
                }
            }
        }
        return {nullptr, ""};
    }

    void mark_provider_success(BackendProvider* prov, const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx);
        if (prov && prov->key_manager) {
            prov->key_manager->mark_success(key);
            prov->status = "ready";
        }
    }

    void mark_provider_failed(BackendProvider* prov, const std::string& key, int status_code) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!prov || !prov->key_manager) return;
        prov->key_manager->mark_failed(key, status_code);
        if (status_code == 401 || status_code == 403) prov->status = "error";
        else prov->status = "cooldown";
    }

    struct ProviderSnapshot {
        std::string name;
        std::string type;
        std::string base_url;
        bool enabled;
        int priority;
        std::string status;
        size_t key_count = 0;
        std::string api_key_masked;
    };

    std::vector<ProviderSnapshot> snapshot() const {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<ProviderSnapshot> out;
        for (const auto& p : providers) {
            ProviderSnapshot s;
            s.name = p.name;
            s.type = p.type;
            s.base_url = p.base_url;
            s.enabled = p.enabled;
            s.priority = p.priority;
            s.status = p.status;
            if (p.key_manager) {
                s.key_count = p.key_manager->get_keys_count();
                auto km_snap = p.key_manager->snapshot();
                if (!km_snap.empty()) {
                    s.api_key_masked = km_snap[0].masked;
                    if (km_snap.size() > 1) {
                        s.api_key_masked += " (+" + std::to_string(km_snap.size() - 1) + " more)";
                    }
                }
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    size_t get_provider_count() const {
        std::lock_guard<std::mutex> lock(mtx);
        return providers.size();
    }
};

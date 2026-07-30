#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>

#include "key_manager.h"
#include "logger.h"

// ============================================================================
// Multi-Provider Backend Management
// ============================================================================

struct BackendProvider {
    std::string name;
    std::string type;           // "nvidia", "openai", "anthropic", "groq", ...
    std::string base_url;
    bool enabled = true;
    int priority = 0;           // lower = higher precedence
    std::string status = "ready"; // ready, cooldown, error, disabled

    // Each provider can have its own key rotation
    std::unique_ptr<KeyManager> key_manager;

    // Optional: per-provider cooldown / health
    std::chrono::steady_clock::time_point last_used{};
};

class ProviderManager {
private:
    std::vector<BackendProvider> providers;
    std::mutex mtx;
    std::vector<std::string> master_keys;
    int base_cooldown = 60;
    int max_cooldown = 1800;

public:
    ProviderManager() = default;

    // Initialize with a default NVIDIA provider using the given keys
    void initialize_default(const std::vector<std::string>& keys,
                            int cooldown = 60,
                            int max_cooldown = 1800) {
        std::lock_guard<std::mutex> lock(mtx);

        master_keys = keys;
        base_cooldown = cooldown;
        this->max_cooldown = max_cooldown;

        providers.clear();

        BackendProvider p;
        p.name = "NVIDIA NIM";
        p.type = "nvidia";
        p.base_url = "https://integrate.api.nvidia.com/v1";
        p.enabled = true;
        p.priority = 0;
        p.status = "ready";

        if (!keys.empty()) {
            p.key_manager = std::make_unique<KeyManager>(keys, cooldown, max_cooldown);
        }

        providers.push_back(std::move(p));
    }

    // Replace the entire provider list (used by TUI when user changes config)
    void set_providers(const std::vector<struct TUIBackendProvider>& ui_providers,
                       const std::vector<std::string>& fallback_keys = {},
                       int cooldown = 60, int max_cooldown = 1800) {
        std::lock_guard<std::mutex> lock(mtx);
        providers.clear();

        // Prefer explicitly passed keys, otherwise use the master keys we were initialized with
        std::vector<std::string> keys_to_use = fallback_keys.empty() ? master_keys : fallback_keys;

        for (const auto& up : ui_providers) {
            if (!up.enabled) continue;

            BackendProvider bp;
            bp.name = up.name;
            bp.type = up.type;
            bp.base_url = up.base_url.empty() ? "https://integrate.api.nvidia.com/v1" : up.base_url;
            bp.enabled = true;
            bp.priority = up.priority;
            bp.status = up.status;

            // Share the same key pool for all providers (common for load balancing demos)
            if (!keys_to_use.empty()) {
                bp.key_manager = std::make_unique<KeyManager>(keys_to_use, cooldown, max_cooldown);
            }

            providers.push_back(std::move(bp));
        }

        // Ensure at least one provider
        if (providers.empty() && !keys_to_use.empty()) {
            BackendProvider bp;
            bp.name = "NVIDIA NIM (fallback)";
            bp.type = "nvidia";
            bp.base_url = "https://integrate.api.nvidia.com/v1";
            bp.enabled = true;
            bp.priority = 999;
            bp.key_manager = std::make_unique<KeyManager>(keys_to_use, cooldown, max_cooldown);
            providers.push_back(std::move(bp));
        }
    }

    // Get a sorted list of currently enabled providers (by priority)
    std::vector<BackendProvider*> get_active_providers() {
        std::lock_guard<std::mutex> lock(mtx);

        std::vector<BackendProvider*> active;
        for (auto& p : providers) {
            if (p.enabled && p.key_manager) {
                active.push_back(&p);
            }
        }

        std::sort(active.begin(), active.end(), [](const BackendProvider* a, const BackendProvider* b) {
            return a->priority < b->priority;
        });

        return active;
    }

    // Get the best (highest priority) provider that is ready
    BackendProvider* get_best_provider() {
        auto actives = get_active_providers();
        if (actives.empty()) return nullptr;
        return actives.front();
    }

    // Returns the next available (provider, key) pair using round-robin across providers + each provider's key rotation.
    // Returns {nullptr, ""} when no providers/keys are available.
    std::pair<BackendProvider*, std::string> get_next_provider_and_key() {
        std::lock_guard<std::mutex> lock(mtx);

        auto active = get_active_providers();  // already sorted by priority
        if (active.empty()) return {nullptr, ""};

        // Try providers in priority order. For each, try to get a key.
        for (auto* prov : active) {
            if (!prov->key_manager) continue;

            std::string key = prov->key_manager->get_key();
            if (!key.empty()) {
                prov->last_used = std::chrono::steady_clock::now();
                return {prov, key};
            }
        }

        // All providers have keys in cooldown — return the soonest one from highest priority provider
        for (auto* prov : active) {
            if (prov->key_manager) {
                // get_key() already handles degraded "soonest" internally, but we call again
                std::string key = prov->key_manager->get_key();
                if (!key.empty()) {
                    prov->last_used = std::chrono::steady_clock::now();
                    return {prov, key};
                }
            }
        }

        return {nullptr, ""};
    }

    void mark_provider_success(BackendProvider* prov, const std::string& key) {
        if (!prov || !prov->key_manager) return;
        prov->key_manager->mark_success(key);
        prov->status = "ready";
    }

    void mark_provider_failed(BackendProvider* prov, const std::string& key, int status_code) {
        if (!prov || !prov->key_manager) return;
        prov->key_manager->mark_failed(key, status_code);

        if (status_code == 401 || status_code == 403) {
            prov->status = "error";
        } else if (status_code == 429) {
            prov->status = "cooldown";
        } else {
            prov->status = "cooldown";
        }
    }

    // Mark success / failure on a specific provider (by name or pointer)
    void mark_success(BackendProvider* prov) {
        if (!prov || !prov->key_manager) return;
        // We don't have the actual key here in this simplified model.
        // In production we would track which key was used.
        prov->status = "ready";
        prov->last_used = std::chrono::steady_clock::now();
    }

    void mark_failed(BackendProvider* prov, int status_code) {
        if (!prov || !prov->key_manager) return;

        // For now we just mark status. Real per-key backoff still lives in KeyManager.
        if (status_code == 429 || status_code == 503) {
            prov->status = "cooldown";
        } else if (status_code == 401 || status_code == 403) {
            prov->status = "error";
        } else {
            prov->status = "cooldown";
        }
    }

    // Snapshot for TUI and /v1/providers endpoint (future)
    struct ProviderSnapshot {
        std::string name;
        std::string type;
        std::string base_url;
        bool enabled;
        int priority;
        std::string status;
        size_t key_count = 0;
    };

    std::vector<ProviderSnapshot> snapshot() {
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

// Lightweight struct used for syncing from TUI
struct TUIBackendProvider {
    std::string name;
    std::string type;
    std::string base_url;
    bool enabled = true;
    int priority = 0;
    std::string status = "ready";
    std::string api_key_masked; // informational only
};

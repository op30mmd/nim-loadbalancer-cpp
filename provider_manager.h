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

    void set_providers(const std::vector<TUIBackendProvider>& ui_providers,
                       const std::vector<std::string>& fallback_keys = {},
                       int cooldown = 60, int max_cooldown = 1800) {
        std::lock_guard<std::mutex> lock(mtx);
        providers.clear();

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

            if (!keys_to_use.empty()) {
                bp.key_manager = std::make_unique<KeyManager>(keys_to_use, cooldown, max_cooldown);
            }
            providers.push_back(std::move(bp));
        }

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
        if (prov && prov->key_manager) {
            prov->key_manager->mark_success(key);
            prov->status = "ready";
        }
    }

    void mark_provider_failed(BackendProvider* prov, const std::string& key, int status_code) {
        if (!prov || !prov->key_manager) return;
        prov->key_manager->mark_failed(key, status_code);
        if (status_code == 401 || status_code == 403) prov->status = "error";
        else if (status_code == 429) prov->status = "cooldown";
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
            if (p.key_manager) s.key_count = p.key_manager->get_keys_count();
            out.push_back(std::move(s));
        }
        return out;
    }

    size_t get_provider_count() const {
        std::lock_guard<std::mutex> lock(mtx);
        return providers.size();
    }
};

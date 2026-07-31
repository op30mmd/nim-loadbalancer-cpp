#include "utils.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstring>

// ============================================================================
// Config & Path Helpers
// ============================================================================

bool file_exists(const std::string& path) {
	std::ifstream f(path.c_str());
	return f.good();
}

std::string get_home_dir() {
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
	if (!home) {
		const char* drive = std::getenv("HOMEDRIVE");
		const char* path = std::getenv("HOMEPATH");
		if (drive && path) {
			return std::string(drive) + std::string(path);
		}
	}
	return home ? std::string(home) : "";
#else
	const char* home = std::getenv("HOME");
	return home ? std::string(home) : "";
#endif
}

// ============================================================================
// Token Estimation & Model Mapping
// ============================================================================

int estimate_input_tokens(const nlohmann::json& anthropic_json) {
	size_t chars = 0;
	auto accumulate_str = [&chars](const std::string& s) { chars += s.size(); };

	if (anthropic_json.contains("system")) {
		if (anthropic_json["system"].is_string()) {
			accumulate_str(anthropic_json["system"].get<std::string>());
		}
		else if (anthropic_json["system"].is_array()) {
			for (const auto& item : anthropic_json["system"]) {
				if (item.is_object() && item.contains("text") && item["text"].is_string()) {
					accumulate_str(item["text"].get<std::string>());
				}
			}
		}
	}

	if (anthropic_json.contains("messages") && anthropic_json["messages"].is_array()) {
		for (const auto& msg : anthropic_json["messages"]) {
			if (msg.contains("content")) {
				if (msg["content"].is_string()) {
					accumulate_str(msg["content"].get<std::string>());
				}
				else if (msg["content"].is_array()) {
					for (const auto& block : msg["content"]) {
						if (block.is_object() && block.contains("text") && block["text"].is_string()) {
							accumulate_str(block["text"].get<std::string>());
						}
					}
				}
			}
		}
	}

	return static_cast<int>((chars + 3) / 4);
}

std::string map_anthropic_model_to_nim(const std::string& anthropic_model) {
	static bool env_loaded = false;
	static nlohmann::json env_override;
	if (!env_loaded) {
		const char* env_map = std::getenv("NIM_MODEL_MAP");
		if (env_map && std::strlen(env_map) > 0) {
			try {
				env_override = nlohmann::json::parse(env_map);
				LOG_INFO("ModelMap", "Loaded model map from NIM_MODEL_MAP env var");
			}
			catch (...) {
				LOG_WARN("ModelMap", "NIM_MODEL_MAP is not valid JSON, ignoring");
			}
		}
		env_loaded = true;
	}

	if (env_override.is_object()) {
		auto it = env_override.find(anthropic_model);
		if (it != env_override.end() && it->is_string()) {
			return it->get<std::string>();
		}
	}

	static const std::unordered_map<std::string, std::string> builtin_map = {
		// Anthropic Claude models
		{"claude-sonnet-4-5-20250916",     "meta/llama-3.1-405b-instruct"},
		{"claude-sonnet-4-5",               "meta/llama-3.1-405b-instruct"},
		{"claude-sonnet-4-20250514",       "meta/llama-3.1-405b-instruct"},
		{"claude-sonnet-4-20250514-smart",  "meta/llama-3.1-405b-instruct"},
		{"claude-sonnet-4",                 "meta/llama-3.1-405b-instruct"},
		{"claude-3-5-sonnet-20241022",      "meta/llama-3.1-70b-instruct"},
		{"claude-3-5-sonnet-latest",        "meta/llama-3.1-70b-instruct"},
		{"claude-3-5-haiku-20241022",      "meta/llama-3.1-8b-instruct"},
		{"claude-3-5-haiku-latest",         "meta/llama-3.1-8b-instruct"},
		{"claude-haiku-4-5-20251001",      "meta/llama-3.1-8b-instruct"},
		{"claude-haiku-4-5",                "meta/llama-3.1-8b-instruct"},
		{"claude-3-opus-20240229",          "meta/llama-3.1-70b-instruct"},
		{"claude-3-sonnet-20240229",        "meta/llama-3.1-70b-instruct"},
		{"claude-3-haiku-20240307",         "meta/llama-3.1-8b-instruct"},
		{"claude-instant-1.2",              "meta/llama-3.1-8b-instruct"},
		// OpenAI models (forwarded to capable NIM equivalents)
		{"gpt-4o",                          "nvidia/nemotron-3-super-120b-a12b"},
		{"gpt-4o-mini",                     "nvidia/nemotron-3-nano-30b-a3b"},
		{"gpt-4-turbo",                     "nvidia/nemotron-3-super-120b-a12b"},
		{"gpt-4",                           "nvidia/nemotron-3-super-120b-a12b"},
		{"gpt-3.5-turbo",                   "nvidia/nemotron-mini-4b-instruct"},
		{"o1",                              "nvidia/nemotron-3-super-120b-a12b"},
		{"o1-mini",                         "nvidia/nemotron-3-nano-30b-a3b"},
		{"o3-mini",                         "nvidia/nemotron-3-nano-30b-a3b"},
		// Non-Anthropic models (passthrough)
		{"z-ai/glm-5.2",                    "z-ai/glm-5.2"},
	};

	auto it = builtin_map.find(anthropic_model);
	if (it != builtin_map.end()) {
		LOG_DEBUG("ModelMap", "Mapped '" + anthropic_model + "' -> '" + it->second + "'");
		return it->second;
	}

	LOG_WARN("ModelMap", "Unknown model '" + anthropic_model + "'. Passed through to NIM as-is. Set NIM_MODEL_MAP env var for custom mapping.");
	return anthropic_model;
}

int get_model_context_window(const std::string& model_id) {
	static const std::unordered_map<std::string, int> context_windows = {
		// Meta Llama
		{"meta/llama-3.1-8b-instruct",              131072},
		{"meta/llama-3.1-70b-instruct",             131072},
		{"meta/llama-3.1-405b-instruct",            131072},
		{"meta/llama-3.3-70b-instruct",             131072},
		{"meta/llama-3.2-1b-instruct",              4096},
		{"meta/llama-3.2-3b-instruct",              4096},
		{"meta/llama-3.2-11b-vision-instruct",      131072},
		{"meta/llama-3.2-90b-vision-instruct",      131072},
		{"meta/llama3-8b-instruct",                 8192},
		{"meta/llama3-70b-instruct",                8192},
		{"meta/llama2-70b",                         4096},
		{"meta/codellama-70b",                      16384},
		// Mistral
		{"mistralai/mistral-7b-instruct-v0.3",     32768},
		{"mistralai/mixtral-8x7b-instruct-v0.1",   32768},
		{"mistralai/mixtral-8x22b-instruct",        32768},
		{"mistralai/mixtral-8x22b-v0.1",           32768},
		{"mistralai/mistral-nemo-12b-instruct",     128000},
		{"mistralai/mistral-large-2-instruct",      128000},
		{"mistralai/mistral-large",                 128000},
		{"mistralai/mistral-nemotron",              128000},
		{"mistralai/codestral-22b-instruct-v0.1",  32768},
		{"nv-mistralai/mistral-nemo-12b-instruct", 128000},
		// NVIDIA
		{"nvidia/llama-3.1-nemotron-70b-instruct",    131072},
		{"nvidia/llama-3.1-nemotron-51b-instruct",    131072},
		{"nvidia/llama-3.1-nemotron-nano-8b-v1",     131072},
		{"nvidia/llama-3.1-nemotron-ultra-253b-v1",  131072},
		{"nvidia/llama-3.3-nemotron-super-49b-v1",   131072},
		{"nvidia/llama-3.3-nemotron-super-49b-v1.5", 131072},
		{"nvidia/llama-3.1-nemotron-nano-vl-8b-v1",  131072},
		{"nvidia/llama-3.2-nemoretriever-1b-vlm-embed-v1", 4096},
		{"nvidia/nemotron-3-nano-30b-a3b",           131072},
		{"nvidia/nemotron-3-nano-omni-30b-a3b-reasoning", 131072},
		{"nvidia/nemotron-3-super-120b-a12b",        131072},
		{"nvidia/nemotron-3-ultra-550b-a55b",        131072},
		{"nvidia/nemotron-mini-4b-instruct",         4096},
		{"nvidia/nemotron-nano-12b-v2-vl",           131072},
		{"nvidia/nemotron-nano-3-30b-a3b",           131072},
		{"nvidia/nemotron-4-340b-instruct",          32768},
		{"nvidia/nemotron-4-340b-reward",            32768},
		{"nvidia/nvidia-nemotron-nano-9b-v2",        131072},
		{"nvidia/cosmos-reason2-8b",                 131072},
		{"nvidia/neva-22b",                          4096},
		// DeepSeek
		{"deepseek/deepseek-r1",                    65536},
		{"deepseek/deepseek-v3-0324",               65536},
		{"deepseek-ai/deepseek-coder-6.7b-instruct", 32768},
		{"deepseek-ai/deepseek-v4-flash",           131072},
		{"deepseek-ai/deepseek-v4-pro",             131072},
		// Google
		{"google/gemma-2b",                         8192},
		{"google/gemma-2-2b-it",                    8192},
		{"google/gemma-2-27b-it",                   8192},
		{"google/gemma-3-4b-it",                    128000},
		{"google/gemma-3-12b-it",                   128000},
		{"google/gemma-4-31b-it",                   128000},
		{"google/codegemma-7b",                     8192},
		{"google/codegemma-1.1-7b",                 8192},
		{"google/recurrentgemma-2b",                4096},
		{"google/diffusiongemma-26b-a4b-it",        8192},
		{"google/deplot",                           4096},
		// Microsoft
		{"microsoft/phi-4-mini-instruct",           128000},
		{"microsoft/phi-4",                         128000},
		{"microsoft/phi-3-vision-128k-instruct",    128000},
		{"microsoft/phi-3.5-moe-instruct",          128000},
		{"microsoft/kosmos-2",                      4096},
		// IBM
		{"ibm/granite-3.0-3b-a800m-instruct",      8192},
		{"ibm/granite-3.0-8b-instruct",            8192},
		{"ibm/granite-34b-code-instruct",           8192},
		{"ibm/granite-8b-code-instruct",            8192},
		// Others
		{"openai/gpt-oss-120b",                     131072},
		{"openai/gpt-oss-20b",                      131072},
		{"z-ai/glm-5.2",                            131072},
		{"moonshotai/kimi-k2.6",                    131072},
		{"stepfun-ai/step-3.7-flash",               131072},
		{"thinkingmachines/inkling",                 131072},
		{"minimaxai/minimax-m3",                    131072},
		{"writer/palmyra-creative-122b",            131072},
		{"writer/palmyra-fin-70b-32k",              32768},
		{"writer/palmyra-med-70b",                  131072},
		{"writer/palmyra-med-70b-32k",              32768},
		{"poolside/laguna-xs-2.1",                  128000},
		{"01-ai/yi-large",                          200000},
		{"ai21labs/jamba-1.5-large-instruct",       256000},
		{"aisingapore/sea-lion-7b-instruct",        4096},
		{"databricks/dbrx-instruct",                32768},
		{"zyphra/zamba2-7b-instruct",               32768},
	};
	auto it = context_windows.find(model_id);
	if (it != context_windows.end()) return it->second;
	return 32768;
}

// ============================================================================
// API Key Loading
// ============================================================================

std::vector<std::string> load_api_keys() {
	std::vector<std::string> keys;

	// Try environment variable (comma-separated)
	const char* env_keys = std::getenv("NVIDIA_API_KEY");
	if (env_keys && env_keys[0] != '\0') {
		std::string s(env_keys);
		std::stringstream ss(s);
		std::string key;
		while (std::getline(ss, key, ',')) {
			key.erase(0, key.find_first_not_of(" \t\r\n"));
			key.erase(key.find_last_not_of(" \t\r\n") + 1);
			if (!key.empty()) keys.push_back(key);
		}
	}

	// Fall back to keys.txt
	if (keys.empty() && file_exists("keys.txt")) {
		LOG_INFO("Startup", "Reading API keys from local 'keys.txt'");
		std::ifstream f("keys.txt");
		std::string line;
		while (std::getline(f, line)) {
			line.erase(0, line.find_first_not_of(" \t\r\n"));
			line.erase(line.find_last_not_of(" \t\r\n") + 1);
			if (!line.empty() && line[0] != '#') {
				keys.push_back(line);
			}
		}
	}

	return keys;
}

std::vector<std::string> load_provider_keys(const std::string& type, const std::vector<std::string>& fallback_keys) {
	std::vector<std::string> keys;
	std::string env_var_name;
	std::string dummy_val;
	if (type == "nvidia") {
		env_var_name = "NVIDIA_API_KEY";
		dummy_val = "nvapi-dummy";
	} else if (type == "openai") {
		env_var_name = "OPENAI_API_KEY";
		dummy_val = "sk-openai-dummy";
	} else if (type == "anthropic") {
		env_var_name = "ANTHROPIC_API_KEY";
		dummy_val = "sk-ant-dummy";
	} else if (type == "google") {
		env_var_name = "GEMINI_API_KEY";
		if (!std::getenv(env_var_name.c_str())) {
			env_var_name = "GOOGLE_API_KEY";
		}
		dummy_val = "sk-gemini-dummy";
	} else if (type == "groq") {
		env_var_name = "GROQ_API_KEY";
		dummy_val = "gsk-groq-dummy";
	} else if (type == "deepseek") {
		env_var_name = "DEEPSEEK_API_KEY";
		dummy_val = "sk-deepseek-dummy";
	} else if (type == "mistral") {
		env_var_name = "MISTRAL_API_KEY";
		dummy_val = "sk-mistral-dummy";
	} else if (type == "together") {
		env_var_name = "TOGETHER_API_KEY";
		dummy_val = "sk-together-dummy";
	} else if (type == "cohere") {
		env_var_name = "COHERE_API_KEY";
		dummy_val = "sk-cohere-dummy";
	} else if (type == "openrouter") {
		env_var_name = "OPENROUTER_API_KEY";
		dummy_val = "sk-openrouter-dummy";
	} else if (type == "ollama") {
		// Ollama doesn't need authorization, but let's give it a dummy key so it is not skipped.
		keys.push_back("ollama-no-key-required");
		return keys;
	}

	if (!env_var_name.empty()) {
		const char* env_keys = std::getenv(env_var_name.c_str());
		if (env_keys && env_keys[0] != '\0') {
			std::string s(env_keys);
			std::stringstream ss(s);
			std::string key;
			while (std::getline(ss, key, ',')) {
				key.erase(0, key.find_first_not_of(" \t\r\n"));
				key.erase(key.find_last_not_of(" \t\r\n") + 1);
				if (!key.empty()) keys.push_back(key);
			}
		}
	}

	if (keys.empty()) {
		// Fallback to type-specific master key logic or dummy key
		if (type == "nvidia" && !fallback_keys.empty()) {
			return fallback_keys;
		}
		// If still empty, use a dummy key so the provider is ready/wired up in the UI
		keys.push_back(dummy_val);
	}
	return keys;
}

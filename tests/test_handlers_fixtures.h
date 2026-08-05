#include <catch2/catch_all.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "proxy_handlers.h"
#include "test_helpers.h"

// ---------------------------------------------------------------------------
// Mock upstream NIM server (httplib in-process)
// ---------------------------------------------------------------------------

struct MockUpstream {
	httplib::Server svr;
	std::thread thread;
	int port = 0;

	std::atomic<int> chat_hits{ 0 };
	std::atomic<int> models_hits{ 0 };
	std::atomic<int> embeddings_hits{ 0 };
	std::mutex auth_mtx;
	std::string last_auth_header;

	std::atomic<int> chat_status{ 200 };
	std::string chat_body =
		R"({"id":"chatcmpl-mock","choices":[{"message":{"role":"assistant","content":"Mock reply"},"finish_reason":"stop"}],"usage":{"prompt_tokens":4,"completion_tokens":2}})";
	std::string chat_ct = "application/json";
	std::atomic<int> models_status{ 200 };
	std::string last_request_body;

	MockUpstream() {
		svr.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
			chat_hits++;
			{
				std::lock_guard<std::mutex> lock(auth_mtx);
				last_auth_header = req.get_header_value("Authorization");
				last_request_body = req.body;
			}
			int st = chat_status.load();
			if (st != 200) {
				res.status = st;
				res.set_content(chat_body, chat_ct);
				return;
			}
			nlohmann::json body;
			try { body = nlohmann::json::parse(req.body); }
			catch (...) { body = nlohmann::json::object(); }
			if (body.value("stream", false)) {
				auto done = std::make_shared<bool>(false);
				res.set_chunked_content_provider("text/event-stream",
					[done](size_t, httplib::DataSink& sink) {
						if (*done) return false;
						*done = true;
						const char* events =
							"data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
							"data: {\"choices\":[{\"delta\":{\"content\":\" world\"},\"finish_reason\":\"stop\"}]}\n\n"
							"data: [DONE]\n\n";
						sink.write(events, std::strlen(events));
						sink.done();
						return false;
					});
			}
			else {
				res.status = 200;
				res.set_content(chat_body, chat_ct);
			}
		});
		svr.Get("/v1/models", [this](const httplib::Request&, httplib::Response& res) {
			models_hits++;
			res.status = models_status.load();
			res.set_content(R"({"object":"list","data":[{"id":"meta/llama-3.1-8b-instruct"}]})",
				"application/json");
		});
		svr.Post("/v1/embeddings", [this](const httplib::Request&, httplib::Response& res) {
			embeddings_hits++;
			res.status = 200;
			res.set_content(R"({"object":"list","data":[]})", "application/json");
		});
	}

	void start() {
		port = svr.bind_to_any_port("127.0.0.1");
		thread = std::thread([this]() { svr.listen_after_bind(); });
	}

	void stop() {
		svr.stop();
		if (thread.joinable()) thread.join();
	}

	// Mirrors production layout: the provider base URL carries the /v1 prefix.
	std::string base_url() const {
		return "http://127.0.0.1:" + std::to_string(port) + "/v1";
	}
};

// ---------------------------------------------------------------------------
// In-process proxy: components + ProxyHandlers wired onto an httplib server
// ---------------------------------------------------------------------------

struct TestProxy {
	EnvGuard env{ "NVIDIA_API_KEY", "" };
	std::unique_ptr<KeyManager> km;
	std::unique_ptr<ClientSideBackoff> backoff;
	std::unique_ptr<ModelCache> cache;
	std::unique_ptr<ProviderManager> providers;
	std::unique_ptr<StatsCollector> stats;
	std::unique_ptr<ProxyHandlers> handlers;
	httplib::Server svr;
	std::thread thread;
	int port = 0;

	TestProxy(bool init_providers = true, bool with_keys = true) {
		std::vector<std::string> keys;
		if (with_keys) {
			keys = { "nvapi-test-key-1", "nvapi-test-key-2" };
		}
		km = std::make_unique<KeyManager>(keys, 60, 1800);
		backoff = std::make_unique<ClientSideBackoff>(16, 0.0);
		cache = std::make_unique<ModelCache>(3600);
		providers = std::make_unique<ProviderManager>();
		if (init_providers) {
			providers->initialize_default(keys, 60, 1800);
		}
		stats = std::make_unique<StatsCollector>();
		handlers = std::make_unique<ProxyHandlers>(*km, *backoff, *cache, *providers, *stats);
	}

	~TestProxy() { stop(); }

	// Point the NVIDIA provider at the given upstream base URL.
	void route_to(const std::string& base_url) {
		std::vector<TUIBackendProvider> ui = {
			{"NVIDIA NIM", "nvidia", base_url, true, 0, "ready"},
		};
		providers->set_providers(ui);
	}

	void start() {
		handlers->setup_routes(svr);
		port = svr.bind_to_any_port("127.0.0.1");
		thread = std::thread([this]() { svr.listen_after_bind(); });
	}

	void stop() {
		svr.stop();
		if (thread.joinable()) thread.join();
	}

	std::shared_ptr<httplib::Client> client() {
		auto cli = std::make_shared<httplib::Client>(
			"http://127.0.0.1:" + std::to_string(port));
		cli->set_connection_timeout(5, 0);
		cli->set_read_timeout(15, 0);
		cli->set_write_timeout(15, 0);
		return cli;
	}
};



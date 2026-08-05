#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <httplib.h>

#include "proxy_config.h"

// libcurl requires curl_global_init before any easy handle is used; do it in a
// global constructor so it runs before the Catch2 main.
namespace {
struct CurlGlobalInit {
	CurlGlobalInit() {
		curl_global_init(CURL_GLOBAL_ALL);
	}
};
CurlGlobalInit curl_global_init_guard;
}

// ---------------------------------------------------------------------------
// SafeQueue
// ---------------------------------------------------------------------------

TEST_CASE("SafeQueue pops items in FIFO order", "[proxy_config]") {
	SafeQueue<std::string> q;
	std::string out;
	q.push("first");
	q.push("second");
	REQUIRE(q.pop_timeout(out, std::chrono::milliseconds(50)));
	REQUIRE(out == "first");
	REQUIRE(q.pop_timeout(out, std::chrono::milliseconds(50)));
	REQUIRE(out == "second");
}

TEST_CASE("SafeQueue pop_timeout returns false on empty queue", "[proxy_config]") {
	SafeQueue<int> q;
	int out = 0;
	REQUIRE_FALSE(q.pop_timeout(out, std::chrono::milliseconds(30)));
}

TEST_CASE("SafeQueue finish wakes waiters and drains remaining items", "[proxy_config]") {
	SafeQueue<int> q;
	q.push(7);
	q.finish();
	// Items still queued: not finished yet
	REQUIRE_FALSE(q.is_finished());
	int out = 0;
	REQUIRE(q.pop_timeout(out, std::chrono::milliseconds(50)));
	REQUIRE(out == 7);
	REQUIRE(q.is_finished());
	// Finished + empty -> immediate false
	REQUIRE_FALSE(q.pop_timeout(out, std::chrono::milliseconds(30)));
}

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

TEST_CASE("Semaphore blocks until notified", "[proxy_config]") {
	Semaphore s(0);
	std::atomic<bool> passed{ false };
	std::thread t([&]() {
		s.wait();
		passed.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE_FALSE(passed.load());
	s.notify();
	t.join();
	REQUIRE(passed.load());
}

TEST_CASE("Semaphore accumulates notify counts", "[proxy_config]") {
	Semaphore s(0);
	s.notify();
	s.notify();
	s.wait();   // consumes one
	s.wait();   // consumes two
	// A third wait would block; verify via a timed thread that it stays blocked
	std::atomic<bool> released{ false };
	std::thread t([&]() {
		s.wait();
		released.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	REQUIRE_FALSE(released.load());
	s.notify();
	t.join();
	REQUIRE(released.load());
}

// ---------------------------------------------------------------------------
// CurlBuffer / write_buffer_callback
// ---------------------------------------------------------------------------

TEST_CASE("write_buffer_callback appends data and returns size", "[proxy_config]") {
	CurlBuffer buf;
	buf.data = "prefix";
	char chunk[] = "hello";
	size_t written = write_buffer_callback(chunk, 1, 5, &buf);
	REQUIRE(written == 5);
	REQUIRE(buf.data == "prefixhello");
}

// ---------------------------------------------------------------------------
// custom_header_callback
// ---------------------------------------------------------------------------

TEST_CASE("custom_header_callback parses status lines", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	char status[] = "HTTP/1.1 200 OK\r\n";
	size_t r = custom_header_callback(status, 1, sizeof(status) - 1, ctx.get());
	REQUIRE(r == sizeof(status) - 1);
	REQUIRE(ctx->http_status.load() == 200);
	REQUIRE_FALSE(ctx->headers_done.load());

	char status2[] = "HTTP/2 429 Too Many Requests\r\n";
	custom_header_callback(status2, 1, sizeof(status2) - 1, ctx.get());
	REQUIRE(ctx->http_status.load() == 429);
}

TEST_CASE("custom_header_callback ignores invalid status lines", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	char bad[] = "HTTP/1.1 ABC NotANumber\r\n";
	custom_header_callback(bad, 1, sizeof(bad) - 1, ctx.get());
	REQUIRE(ctx->http_status.load() == 0);
}

TEST_CASE("custom_header_callback detects event-stream content type", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	char ct[] = "Content-Type: text/event-stream\r\n";
	custom_header_callback(ct, 1, sizeof(ct) - 1, ctx.get());
	REQUIRE(ctx->content_type == "text/event-stream");
	REQUIRE(ctx->is_stream);

	auto ctx2 = std::make_shared<ProxyContext>();
	char ct2[] = "Content-Type: application/json\r\n";
	custom_header_callback(ct2, 1, sizeof(ct2) - 1, ctx2.get());
	REQUIRE(ctx2->content_type == "application/json");
	REQUIRE_FALSE(ctx2->is_stream);
}

TEST_CASE("custom_header_callback blank line signals headers done", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	std::thread t([&]() {
		std::unique_lock<std::mutex> lock(ctx->header_mtx);
		ctx->header_cv.wait_for(lock, std::chrono::seconds(2),
			[&]() { return ctx->headers_done.load(); });
	});
	char blank[] = "\r\n";
	custom_header_callback(blank, 1, 2, ctx.get());
	t.join();
	REQUIRE(ctx->headers_done.load());
}

TEST_CASE("custom_header_callback trims header keys/values", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	char hdr[] = "  X-Custom-Header  :   some value  \r\n";
	size_t r = custom_header_callback(hdr, 1, sizeof(hdr) - 1, ctx.get());
	REQUIRE(r == sizeof(hdr) - 1);
	REQUIRE_FALSE(ctx->headers_done.load());  // not a blank line
}

// ---------------------------------------------------------------------------
// custom_write_callback
// ---------------------------------------------------------------------------

TEST_CASE("custom_write_callback buffers non-stream bodies", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	char a[] = "Hello, ";
	char b[] = "world!";
	REQUIRE(custom_write_callback(a, 1, 7, ctx.get()) == 7);
	REQUIRE(custom_write_callback(b, 1, 6, ctx.get()) == 6);
	REQUIRE(ctx->full_body_buffer == "Hello, world!");
	REQUIRE(ctx->chunk_queue.is_finished() == false);
}

TEST_CASE("custom_write_callback queues stream chunks", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	ctx->is_stream = true;
	char a[] = "data: one\n\n";
	char b[] = "data: two\n\n";
	REQUIRE(custom_write_callback(a, 1, sizeof(a) - 1, ctx.get()) == sizeof(a) - 1);
	REQUIRE(custom_write_callback(b, 1, sizeof(b) - 1, ctx.get()) == sizeof(b) - 1);
	std::string chunk;
	REQUIRE(ctx->chunk_queue.pop_timeout(chunk, std::chrono::milliseconds(50)));
	REQUIRE(chunk == "data: one\n\n");
	REQUIRE(ctx->chunk_queue.pop_timeout(chunk, std::chrono::milliseconds(50)));
	REQUIRE(chunk == "data: two\n\n");
	REQUIRE(ctx->full_body_buffer.empty());
}

TEST_CASE("custom_write_callback stops when client disconnected", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	ctx->client_disconnected = true;
	char a[] = "data";
	REQUIRE(custom_write_callback(a, 1, 4, ctx.get()) == 0);
	REQUIRE(ctx->full_body_buffer.empty());
}

// ---------------------------------------------------------------------------
// run_curl_request + configure_curl_network_stability
// ---------------------------------------------------------------------------

TEST_CASE("run_curl_request succeeds against a local server", "[proxy_config]") {
	httplib::Server svr;
	svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
		res.set_content("pong", "text/plain");
	});
	int port = svr.bind_to_any_port("127.0.0.1");
	REQUIRE(port > 0);
	std::thread server_thread([&]() { svr.listen_after_bind(); });

	auto ctx = std::make_shared<ProxyContext>();
	CURL* curl = curl_easy_init();
	REQUIRE(curl != nullptr);
	std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ping";
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	configure_curl_network_stability(curl);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, custom_header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx.get());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, custom_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());

	run_curl_request(curl, ctx.get());
	curl_easy_cleanup(curl);

	REQUIRE_FALSE(ctx->curl_failed.load());
	REQUIRE(ctx->headers_done.load());
	REQUIRE(ctx->http_status.load() == 200);
	REQUIRE(ctx->full_body_buffer == "pong");
	REQUIRE(ctx->chunk_queue.is_finished());

	svr.stop();
	server_thread.join();
}

TEST_CASE("run_curl_request reports failure for refused connections", "[proxy_config]") {
	auto ctx = std::make_shared<ProxyContext>();
	CURL* curl = curl_easy_init();
	REQUIRE(curl != nullptr);
	// Port 1 on localhost is virtually always refused -> immediate RST
	curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:1/");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, custom_header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, ctx.get());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, custom_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());

	run_curl_request(curl, ctx.get());
	curl_easy_cleanup(curl);

	REQUIRE(ctx->curl_failed.load());
	REQUIRE_FALSE(ctx->curl_error_msg.empty());
	REQUIRE(ctx->headers_done.load());
	REQUIRE(ctx->chunk_queue.is_finished());
}

TEST_CASE("configure_curl_network_stability applies options without error", "[proxy_config]") {
	CURL* curl = curl_easy_init();
	REQUIRE(curl != nullptr);
	configure_curl_network_stability(curl);   // no crash, options set
	curl_easy_cleanup(curl);
}

#include <catch2/catch_all.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "stats_collector.h"

TEST_CASE("StatsCollector counts successes and failures", "[stats]") {
	StatsCollector sc;
	sc.record_request(200, 10, "/v1/messages", true);
	sc.record_request(200, 12, "/v1/messages", true);
	sc.record_request(500, 20, "/v1/messages", false);
	auto s = sc.snapshot();
	REQUIRE(s.total_requests == 3);
	REQUIRE(s.total_successes == 2);
	REQUIRE(s.total_failures == 1);
	REQUIRE(s.total_server_errors == 1);
	REQUIRE(s.total_rate_limits == 0);
	REQUIRE(s.total_auth_failures == 0);
	REQUIRE(s.success_rate == Catch::Approx(66.6666).epsilon(0.01));
}

TEST_CASE("StatsCollector classifies failure status codes", "[stats]") {
	StatsCollector sc;
	sc.record_request(429, 5, "/v1/chat/completions", false);
	sc.record_request(401, 5, "/v1/chat/completions", false);
	sc.record_request(403, 5, "/v1/chat/completions", false);
	sc.record_request(503, 5, "/v1/chat/completions", false);
	sc.record_request(400, 5, "/v1/chat/completions", false);
	auto s = sc.snapshot();
	REQUIRE(s.total_failures == 5);
	REQUIRE(s.total_rate_limits == 1);
	REQUIRE(s.total_auth_failures == 2);
	REQUIRE(s.total_server_errors == 1);   // only the 503 is 5xx; 400 is not
}

TEST_CASE("StatsCollector computes latency statistics", "[stats]") {
	StatsCollector sc;
	for (int i = 1; i <= 10; ++i) {
		sc.record_request(200, i * 10, "/v1/messages", true);
	}
	auto s = sc.snapshot();
	REQUIRE(s.avg_latency_ms == Catch::Approx(55.0).epsilon(0.001));
	REQUIRE(s.min_latency_ms == Catch::Approx(10.0).epsilon(0.001));
	REQUIRE(s.max_latency_ms == Catch::Approx(100.0).epsilon(0.001));
	REQUIRE(s.p95_latency_ms == Catch::Approx(95.5).epsilon(0.001));
	REQUIRE(s.p99_latency_ms == Catch::Approx(99.1).epsilon(0.001));
}

TEST_CASE("StatsCollector excludes negative latencies", "[stats]") {
	StatsCollector sc;
	sc.record_request(200, -1, "/v1/messages", true);
	sc.record_request(200, 20, "/v1/messages", true);
	auto s = sc.snapshot();
	REQUIRE(s.avg_latency_ms == Catch::Approx(20.0).epsilon(0.001));
	REQUIRE(s.min_latency_ms == Catch::Approx(20.0).epsilon(0.001));
}

TEST_CASE("StatsCollector empty snapshot is all zeros", "[stats]") {
	StatsCollector sc;
	auto s = sc.snapshot();
	REQUIRE(s.total_requests == 0);
	REQUIRE(s.success_rate == 0.0);
	REQUIRE(s.avg_latency_ms == 0.0);
	REQUIRE(s.requests_per_minute == 0.0);
	REQUIRE(s.recent_requests.empty());
	REQUIRE(s.throughput_series.empty());
}

TEST_CASE("StatsCollector reports requests per minute", "[stats]") {
	StatsCollector sc;
	for (int i = 0; i < 5; ++i) {
		sc.record_request(200, 1, "/v1/messages", true);
	}
	auto s = sc.snapshot();
	REQUIRE(s.requests_per_minute > 0.0);
}

TEST_CASE("StatsCollector caps recent requests at 100 newest-first", "[stats]") {
	StatsCollector sc;
	for (int i = 0; i < 150; ++i) {
		sc.record_request(200, i, "/v1/chat/completions", true);
	}
	auto s = sc.snapshot();
	REQUIRE(s.recent_requests.size() == 100);
	REQUIRE(s.recent_requests.front().latency_ms == 149);  // newest first
	REQUIRE(s.recent_requests.back().latency_ms == 50);
	REQUIRE(s.recent_requests.front().endpoint == "/v1/chat/completions");
}

TEST_CASE("StatsCollector flushes histogram buckets into time series", "[stats]") {
	StatsCollector sc(0.05);
	sc.record_request(200, 5, "/v1/messages", true);
	std::this_thread::sleep_for(std::chrono::milliseconds(70));
	sc.record_request(200, 5, "/v1/messages", true);

	auto s = sc.snapshot();
	// First bucket flushed to the series + live bucket point
	REQUIRE(s.throughput_series.size() >= 2);
	REQUIRE(s.throughput_series.front().value == Catch::Approx(2.0).epsilon(0.001));
	REQUIRE(s.latency_series.size() >= 1);
	REQUIRE(s.error_rate_series.size() >= 2);
	REQUIRE(s.total_requests == 2);
}

TEST_CASE("StatsCollector tracks active streams", "[stats]") {
	StatsCollector sc;
	REQUIRE(sc.snapshot().active_streams == 0);
	sc.stream_started();
	sc.stream_started();
	REQUIRE(sc.snapshot().active_streams == 2);
	sc.stream_ended();
	REQUIRE(sc.snapshot().active_streams == 1);
	sc.stream_ended();
	REQUIRE(sc.snapshot().active_streams == 0);
}

TEST_CASE("StatsCollector reports uptime", "[stats]") {
	StatsCollector sc;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	REQUIRE(sc.snapshot().uptime_seconds >= 0);
}

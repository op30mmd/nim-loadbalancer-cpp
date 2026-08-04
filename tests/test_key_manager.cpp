#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "key_manager.h"

// ---------------------------------------------------------------------------
// KeyManager
// ---------------------------------------------------------------------------

TEST_CASE("KeyManager empty pool returns empty key", "[key_manager]") {
	KeyManager km({});
	REQUIRE(km.get_key().empty());
	REQUIRE(km.get_keys_count() == 0);
	REQUIRE(km.get_first_key().empty());
	REQUIRE(km.snapshot().empty());
	// No crash on failure/success reporting for unknown keys
	km.mark_failed("nope", 429);
	km.mark_success("nope");
	REQUIRE(km.snapshot().empty());
}

TEST_CASE("KeyManager round-robins across the pool", "[key_manager]") {
	KeyManager km({"keyA", "keyB", "keyC"});
	REQUIRE(km.get_key() == "keyA");
	REQUIRE(km.get_key() == "keyB");
	REQUIRE(km.get_key() == "keyC");
	REQUIRE(km.get_key() == "keyA");
	REQUIRE(km.get_keys_count() == 3);
	REQUIRE(km.get_first_key() == "keyA");
}

TEST_CASE("KeyManager masks keys in snapshots", "[key_manager]") {
	KeyManager km({"abcdefghij", "abc"});
	auto snap = km.snapshot();
	REQUIRE(snap.size() == 2);
	REQUIRE(snap[0].masked == "...efghij");
	REQUIRE(snap[1].masked == "abc");
	REQUIRE(snap[0].state == "available");
}

TEST_CASE("KeyManager rate-limit failure sets cooldown and counts", "[key_manager]") {
	KeyManager km({"keyA", "keyB"}, 60, 1800);
	km.get_key();  // consumes keyA
	km.mark_failed("keyA", 429);

	auto snap = km.snapshot();
	REQUIRE(snap[0].state == "cooldown");
	REQUIRE(snap[0].consecutive_failures == 1);
	REQUIRE(snap[0].total_failures == 1);
	REQUIRE(snap[0].total_requests == 1);
	REQUIRE(snap[0].cooldown_remaining_sec > 0);
	REQUIRE(snap[0].cooldown_remaining_sec <= 60);
}

TEST_CASE("KeyManager failure classes get different base cooldowns", "[key_manager]") {
	// AUTH (401/403): base of at least 300s (capped by max_cooldown)
	{
		KeyManager km({"keyA"}, 60, 1800);
		km.mark_failed("keyA", 401);
		auto s = km.snapshot()[0];
		REQUIRE(s.cooldown_remaining_sec >= 299);
		REQUIRE(s.cooldown_remaining_sec <= 300);
	}
	// SERVER (5xx): base/4, minimum 5
	{
		KeyManager km({"keyA"}, 60, 1800);
		km.mark_failed("keyA", 503);
		auto s = km.snapshot()[0];
		REQUIRE(s.cooldown_remaining_sec >= 14);
		REQUIRE(s.cooldown_remaining_sec <= 15);
	}
	// NETWORK (negative) and UNKNOWN use the base cooldown
	{
		KeyManager km({"keyA"}, 60, 1800);
		km.mark_failed("keyA", -1);
		REQUIRE(km.snapshot()[0].cooldown_remaining_sec <= 60);
		REQUIRE(km.snapshot()[0].cooldown_remaining_sec >= 59);
	}
	{
		KeyManager km({"keyA"}, 60, 1800);
		km.mark_failed("keyA", 400);
		REQUIRE(km.snapshot()[0].cooldown_remaining_sec <= 60);
		REQUIRE(km.snapshot()[0].cooldown_remaining_sec >= 59);
	}
	// AUTH capped by max_cooldown
	{
		KeyManager km({"keyA"}, 60, 100);
		km.mark_failed("keyA", 403);
		auto s = km.snapshot()[0];
		REQUIRE(s.cooldown_remaining_sec > 0);
		REQUIRE(s.cooldown_remaining_sec <= 100);
	}
}

TEST_CASE("KeyManager backoff doubles per consecutive failure and caps", "[key_manager]") {
	KeyManager km({"keyA"}, 60, 1800);
	km.mark_failed("keyA", 429);
	long long first = km.snapshot()[0].cooldown_remaining_sec;
	REQUIRE(first > 0);
	REQUIRE(first <= 60);

	km.mark_failed("keyA", 429);
	long long second = km.snapshot()[0].cooldown_remaining_sec;
	REQUIRE(second > first);   // 120 vs 60
	REQUIRE(second <= 120);

	km.mark_failed("keyA", 429);
	long long third = km.snapshot()[0].cooldown_remaining_sec;
	REQUIRE(third > second);   // 240 vs 120
	REQUIRE(third <= 240);

	REQUIRE(km.snapshot()[0].consecutive_failures == 3);
	REQUIRE(km.snapshot()[0].total_failures == 3);
}

TEST_CASE("KeyManager backoff exponent is clamped and capped", "[key_manager]") {
	KeyManager km({"keyA"}, 60, 1800);
	for (int i = 0; i < 10; ++i) {
		km.mark_failed("keyA", 429);
	}
	auto s = km.snapshot()[0];
	REQUIRE(s.consecutive_failures == 10);
	// 60 << 7 = 7680 -> capped at 1800
	REQUIRE(s.cooldown_remaining_sec > 1799 - 1);
	REQUIRE(s.cooldown_remaining_sec <= 1800);
}

TEST_CASE("KeyManager success resets streak and clears cooldown", "[key_manager]") {
	KeyManager km({"keyA"}, 60, 1800);
	km.mark_failed("keyA", 429);
	km.mark_failed("keyA", 429);
	REQUIRE(km.snapshot()[0].consecutive_failures == 2);
	REQUIRE(km.snapshot()[0].state == "cooldown");

	km.mark_success("keyA");
	auto s = km.snapshot()[0];
	REQUIRE(s.consecutive_failures == 0);
	REQUIRE(s.total_successes == 1);
	REQUIRE(s.state == "available");
	REQUIRE(s.cooldown_remaining_sec == 0);
}

TEST_CASE("KeyManager skips cooling-down keys in round robin", "[key_manager]") {
	KeyManager km({"keyA", "keyB"}, 60, 1800);
	REQUIRE(km.get_key() == "keyA");
	km.mark_failed("keyA", 429);

	// keyA is cooling down -> keyB is served
	REQUIRE(km.get_key() == "keyB");
}

TEST_CASE("KeyManager degrades to soonest-available key when all cool down", "[key_manager]") {
	KeyManager km({ "keyA", "keyB" }, 60, 1800);
	REQUIRE(km.get_key() == "keyA");
	km.mark_failed("keyA", 401);   // 300s cooldown
	REQUIRE(km.get_key() == "keyB");
	km.mark_failed("keyB", 429);   // 60s cooldown

	// Both cooling down: keyB expires sooner and is handed out
	REQUIRE(km.get_key() == "keyB");
	// Requests are still counted on the degraded pick
	REQUIRE(km.snapshot()[1].total_requests == 2);
}

TEST_CASE("KeyManager status_summary format", "[key_manager]") {
	KeyManager km({"abcdefghij"}, 60, 1800);
	std::string summary = km.status_summary();
	REQUIRE(summary.find("1 keys |") == 0);
	REQUIRE(summary.find("...efghij:available(0f/0u)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ClientSideBackoff
// ---------------------------------------------------------------------------

TEST_CASE("ClientSideBackoff enforces minimum inter-request interval", "[key_manager]") {
	ClientSideBackoff b(4, 0.1);
	b.acquire();
	auto t0 = std::chrono::steady_clock::now();
	b.acquire();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();
	b.release();
	b.release();
	REQUIRE(elapsed >= 90);
}

TEST_CASE("ClientSideBackoff limits concurrency", "[key_manager]") {
	ClientSideBackoff b(1, 0.0);
	std::atomic<bool> entered{ false };
	std::atomic<bool> exited{ false };

	b.acquire();  // take the single slot

	std::thread t([&]() {
		b.acquire();
		entered.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		exited.store(true);
		b.release();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	REQUIRE_FALSE(entered.load());   // thread still blocked on the semaphore
	b.release();                     // free the slot
	t.join();
	REQUIRE(entered.load());
	REQUIRE(exited.load());
}

TEST_CASE("ClientSideBackoff global backoff throttles requests", "[key_manager]") {
	ClientSideBackoff b(4, 0.0);
	b.trigger_global_backoff(0.15);
	auto t0 = std::chrono::steady_clock::now();
	b.acquire();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();
	b.release();
	REQUIRE(elapsed >= 130);
}

TEST_CASE("ClientSideBackoff release without acquire is safe", "[key_manager]") {
	ClientSideBackoff b(2, 0.0);
	b.release();
	b.acquire();
	b.release();
}

// ---------------------------------------------------------------------------
// ModelCache
// ---------------------------------------------------------------------------

TEST_CASE("ModelCache stores, serves and overwrites data", "[key_manager]") {
	ModelCache cache(3600);
	REQUIRE(cache.get().empty());
	cache.set("{\"models\":[]}");
	REQUIRE(cache.get() == "{\"models\":[]}");
	cache.set("{\"models\":[1]}");
	REQUIRE(cache.get() == "{\"models\":[1]}");
}

TEST_CASE("ModelCache expires entries after TTL", "[key_manager]") {
	ModelCache cache(0.05);
	cache.set("stale");
	REQUIRE(cache.get() == "stale");
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	REQUIRE(cache.get().empty());
}

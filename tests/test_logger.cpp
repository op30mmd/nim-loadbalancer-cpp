// Tests for the Logger: file output and the TUI-mode callback dispatch.
// (The TUI itself — tui_panel.h — is out of scope; this only exercises the
// Logger's public API hooks that the TUI integration uses.)
#include <catch2/catch_all.hpp>

#include <fstream>
#include <string>

#include "logger.h"
#include "test_helpers.h"

static void remove_test_file(const std::string& path) {
	std::remove(path.c_str());
}

TEST_CASE("logger writes plain lines to the configured file", "[logger]") {
	const char* log_path = "nim-test-logger.log";
	remove_test_file(log_path);
	{
		Logger logger;
		logger.init(LogLevel::LEVEL_DEBUG, log_path);
		logger.log(LogLevel::LEVEL_INFO, "TestTag", "hello from logger test");
		logger.log(LogLevel::LEVEL_DEBUG, "TestTag", "debug detail");
	}
	std::ifstream f(log_path);
	REQUIRE(f.good());
	std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	REQUIRE(content.find("[INFO ]") != std::string::npos);
	REQUIRE(content.find("[TestTag]") != std::string::npos);
	REQUIRE(content.find("hello from logger test") != std::string::npos);
	REQUIRE(content.find("debug detail") != std::string::npos);
	remove_test_file(log_path);
}

TEST_CASE("logger dispatches to the tui callback when tui mode is active", "[logger]") {
	Logger logger;
	bool called = false;
	std::string received;
	logger.set_tui_mode(true, [&](const std::string& line) {
		called = true;
		received = line;
	});
	logger.log(LogLevel::LEVEL_WARN, "TuiTag", "tui payload");
	REQUIRE(called);
	REQUIRE(received.find("[TuiTag]") != std::string::npos);
	REQUIRE(received.find("tui payload") != std::string::npos);
	logger.set_tui_mode(false);
}

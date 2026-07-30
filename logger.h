#pragma once

#include <iostream>
#include <string>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <functional>

enum class LogLevel {
	LEVEL_DEBUG,
	LEVEL_INFO,
	LEVEL_WARN,
	LEVEL_ERROR
};

class Logger {
private:
	std::mutex mtx;
	std::ofstream log_file;
	LogLevel min_level = LogLevel::LEVEL_DEBUG;
	bool file_enabled = false;
	bool tui_mode = false;
	std::function<void(const std::string&)> tui_callback;

public:
	void init(LogLevel level = LogLevel::LEVEL_DEBUG, const std::string& filename = "proxy.log") {
		min_level = level;
		if (!filename.empty()) {
			log_file.open(filename, std::ios::out | std::ios::app);
			if (log_file.is_open()) {
				file_enabled = true;
			}
		}
	}

	void set_tui_mode(bool enabled, std::function<void(const std::string&)> callback = nullptr) {
		std::unique_lock<std::mutex> lock(mtx);
		tui_mode = enabled;
		tui_callback = callback;
	}

	void log(LogLevel level, const std::string& tag, const std::string& message) {
		if (level < min_level) return;

		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

		std::stringstream ss;
		struct tm time_info;
#ifdef _WIN32
		localtime_s(&time_info, &in_time_t);
#else
		localtime_r(&in_time_t, &time_info);
#endif
		ss << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S")
			<< '.' << std::setfill('0') << std::setw(3) << ms.count();

		std::string time_str = ss.str();
		std::string level_str;
		std::string color_code = "";
		std::string reset_code = "\033[0m";

		switch (level) {
		case LogLevel::LEVEL_DEBUG:
			level_str = "DEBUG";
			color_code = "\033[36m";
			break;
		case LogLevel::LEVEL_INFO:
			level_str = "INFO ";
			color_code = "\033[32m";
			break;
		case LogLevel::LEVEL_WARN:
			level_str = "WARN ";
			color_code = "\033[33m";
			break;
		case LogLevel::LEVEL_ERROR:
			level_str = "ERROR";
			color_code = "\033[31m";
			break;
		}

		std::string plain_line = "[" + time_str + "] [" + level_str + "] [" + tag + "] " + message;
		std::string colored_line = "[" + time_str + "] [" + color_code + level_str + reset_code + "] [" + tag + "] " + message;

		{
			std::unique_lock<std::mutex> lock(mtx);

			// In TUI mode, suppress stdout (the TUI owns the screen)
			if (!tui_mode) {
				std::cout << colored_line << "\n";
			}

			if (file_enabled && log_file.is_open()) {
				log_file << plain_line << "\n";
				log_file.flush();
			}

			// Feed to TUI callback
			if (tui_mode && tui_callback) {
				tui_callback(colored_line);
			}
		}
	}
};

inline Logger g_logger;

#define LOG_DEBUG(tag, msg) g_logger.log(LogLevel::LEVEL_DEBUG, tag, msg)
#define LOG_INFO(tag, msg)  g_logger.log(LogLevel::LEVEL_INFO, tag, msg)
#define LOG_WARN(tag, msg)  g_logger.log(LogLevel::LEVEL_WARN, tag, msg)
#define LOG_ERROR(tag, msg) g_logger.log(LogLevel::LEVEL_ERROR, tag, msg)

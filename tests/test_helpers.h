#pragma once

// Shared helpers for the test suite.

#include <cstdlib>
#include <string>

static void nim_set_env(const char* name, const char* value) {
#ifdef _WIN32
	_putenv_s(name, value ? value : "");
#else
	if (value) setenv(name, value, 1);
	else unsetenv(name);
#endif
}

// RAII: sets an env var for the duration of a test, restoring the previous
// value (or unsetting it) afterwards.
class EnvGuard {
public:
	EnvGuard(const char* name, const char* value) : name_(name) {
		const char* prev = std::getenv(name);
		had_prev_ = (prev != nullptr);
		if (had_prev_) prev_ = prev;
		nim_set_env(name, value);
	}
	~EnvGuard() {
		if (had_prev_) nim_set_env(name_.c_str(), prev_.c_str());
		else nim_set_env(name_.c_str(), nullptr);
	}
private:
	std::string name_;
	std::string prev_;
	bool had_prev_ = false;
};

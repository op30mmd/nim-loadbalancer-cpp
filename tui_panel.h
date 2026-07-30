#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <deque>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#endif

#include "stats_collector.h"
#include "key_manager.h"
#include "provider_manager.h"

// ============================================================================
// Terminal Helpers
// ============================================================================

namespace term {

inline int get_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
#endif
}

inline int get_height() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 24;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return w.ws_row;
    return 24;
#endif
}

#ifndef _WIN32
static struct termios orig_termios;
static bool raw_mode_active = false;

inline void disable_raw_mode() {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
        setvbuf(stdout, nullptr, _IOLBF, 1024);
    }
}

inline void enable_raw_mode() {
    if (!raw_mode_active) {
        setvbuf(stdout, nullptr, _IOFBF, 65536);
        tcgetattr(STDIN_FILENO, &orig_termios);
        atexit(disable_raw_mode);
        struct termios raw = orig_termios;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        raw_mode_active = true;
    }
}

inline int read_key() {
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == '\x1b') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 1001; // UP
                case 'B': return 1002; // DOWN
                case 'C': return 1003; // RIGHT
                case 'D': return 1004; // LEFT
            }
        }
        return '\x1b';
    }
    return c;
}
#else
inline void enable_raw_mode() {
    setvbuf(stdout, nullptr, _IOFBF, 65536);
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(h, &mode);
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | 0x0010);
    SetConsoleMode(h, mode);
    // Enable virtual terminal processing for ANSI codes, disable auto-wrap
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode;
    GetConsoleMode(out, &out_mode);
    out_mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    out_mode &= ~0x0002; // Disable ENABLE_WRAP_AT_EOL_OUTPUT to prevent auto-wrap scrolling
    SetConsoleMode(out, out_mode);
    SetConsoleOutputCP(CP_UTF8);
    CONSOLE_CURSOR_INFO cci;
    if (GetConsoleCursorInfo(out, &cci)) {
        cci.bVisible = FALSE;
        SetConsoleCursorInfo(out, &cci);
    }
}

inline void disable_raw_mode() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(h, &mode);
    mode |= (ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | 0x0010);
    SetConsoleMode(h, mode);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cci;
    if (GetConsoleCursorInfo(out, &cci)) {
        cci.bVisible = TRUE;
        SetConsoleCursorInfo(out, &cci);
    }
    setvbuf(stdout, nullptr, _IOLBF, 1024);
}

inline int read_key() {
    if (!_kbhit()) return -1;
    int c = _getch();
    if (c == 0 || c == 224) {
        c = _getch();
        switch (c) {
            case 72: return 1001;
            case 80: return 1002;
            case 77: return 1003;
            case 75: return 1004;
        }
        return -1;
    }
    return c;
}
#endif

} // namespace term

// ============================================================================
// ANSI Escape Helpers
// ============================================================================

namespace ansi {
    const char* const RESET   = "\033[0m";
    const char* const BOLD    = "\033[1m";
    const char* const DIM     = "\033[2m";
    const char* const HIDE_CURSOR = "\033[?25l";
    const char* const SHOW_CURSOR = "\033[?25h";
    const char* const CLEAR_SCREEN = "\033[2J";
    const char* const CLEAR_EOL = "\033[K";
    const char* const CLEAR_EOS = "\033[J";

    // Colors
    const char* const BLACK   = "\033[30m";
    const char* const RED     = "\033[31m";
    const char* const GREEN   = "\033[32m";
    const char* const YELLOW  = "\033[33m";
    const char* const BLUE    = "\033[34m";
    const char* const MAGENTA = "\033[35m";
    const char* const CYAN    = "\033[36m";
    const char* const WHITE   = "\033[37m";
    const char* const GRAY    = "\033[90m";

    // Bright
    const char* const BRIGHT_RED    = "\033[91m";
    const char* const BRIGHT_GREEN  = "\033[92m";
    const char* const BRIGHT_YELLOW = "\033[93m";
    const char* const BRIGHT_BLUE   = "\033[94m";
    const char* const BRIGHT_CYAN   = "\033[96m";
    const char* const BRIGHT_WHITE  = "\033[97m";

    // Background
    const char* const BG_RED    = "\033[41m";
    const char* const BG_GREEN  = "\033[42m";
    const char* const BG_YELLOW = "\033[43m";
    const char* const BG_BLUE   = "\033[44m";
    const char* const BG_CYAN   = "\033[46m";
    const char* const BG_GRAY   = "\033[100m";

    inline std::string move(int row, int col) {
        return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
    }
} // namespace ansi

// ============================================================================
// TUI Rendering Utilities
// ============================================================================

inline std::string format_duration(long long seconds) {
    long long h = seconds / 3600;
    long long m = (seconds % 3600) / 60;
    long long s = seconds % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%lldh %02lldm %02llds", h, m, s);
    else if (m > 0) snprintf(buf, sizeof(buf), "%lldm %02llds", m, s);
    else snprintf(buf, sizeof(buf), "%llds", s);
    return std::string(buf);
}

inline std::string format_number(double val) {
    char buf[32];
    if (val >= 1000000.0) snprintf(buf, sizeof(buf), "%.1fM", val / 1000000.0);
    else if (val >= 1000.0) snprintf(buf, sizeof(buf), "%.1fK", val / 1000.0);
    else if (val >= 100.0) snprintf(buf, sizeof(buf), "%.0f", val);
    else if (val >= 10.0) snprintf(buf, sizeof(buf), "%.1f", val);
    else snprintf(buf, sizeof(buf), "%.2f", val);
    return std::string(buf);
}

inline std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return s.substr(0, max_len - 3) + "...";
}

inline std::string truncate_ansi(const std::string& s, size_t max_visible) {
    size_t visible_count = 0;
    bool in_escape = false;
    size_t cut_idx = s.size();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) {
                in_escape = false;
            }
            continue;
        }
        visible_count++;
        if (visible_count == max_visible - 3 && cut_idx == s.size()) {
            cut_idx = i + 1;
        }
        if (visible_count >= max_visible) {
            return s.substr(0, cut_idx) + "..." + ansi::RESET;
        }
    }
    return s;
}

inline size_t visible_length(const std::string& s) {
    size_t count = 0;
    bool in_escape = false;
    for (char c : s) {
        if (c == '\033') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                in_escape = false;
            }
            continue;
        }
        count++;
    }
    return count;
}

inline std::string pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

inline std::string pad_left(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return std::string(width - s.size(), ' ') + s;
}

// Draw a horizontal bar chart using clean ASCII characters (100% portable across Windows/Unix terminals)
inline std::string bar(double value, double max_val, int width,
                       const char* color = ansi::BRIGHT_CYAN) {
    if (max_val <= 0 || value <= 0 || width <= 0) return std::string(width, '-');
    double ratio = (std::min)(value / max_val, 1.0);
    int filled = (int)(ratio * width);

    std::string result = std::string(color);
    for (int i = 0; i < filled && i < width; i++) result += '#'; // full block
    result += ansi::RESET;
    if (filled < width) result += std::string(width - filled, '-');
    return result;
}

// Draw a sparkline from a time series using clean ASCII characters
inline std::string sparkline(const std::vector<TimeSeriesPoint>& series, int width,
                             const char* color = ansi::BRIGHT_CYAN) {
    static const char chars[] = {'_', '.', '-', '=', '*', '#'};

    if (series.empty() || width <= 0) return std::string(width, '_');

    // Take last `width` points
    size_t start = series.size() > (size_t)width ? series.size() - width : 0;
    std::vector<double> vals;
    for (size_t i = start; i < series.size(); i++) {
        vals.push_back(series[i].value);
    }

    double max_val = 0;
    for (double v : vals) if (v > max_val) max_val = v;
    if (max_val == 0) max_val = 1;

    std::string result = std::string(color);
    for (size_t i = 0; i < (size_t)width && i < vals.size(); i++) {
        int idx = (int)(vals[i] / max_val * 5.0);
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
        result += chars[idx];
    }
    result += ansi::RESET;
    if (vals.size() < (size_t)width) {
        result += std::string(width - vals.size(), '_');
    }
    return result;
}

// Draw an ASCII area chart (multi-row)
inline std::vector<std::string> area_chart(const std::vector<TimeSeriesPoint>& series,
                                            int width, int height,
                                            const char* color = ansi::BRIGHT_CYAN,
                                            const char* fill_color = ansi::CYAN) {
    std::vector<std::string> rows(height, std::string(width, ' '));
    if (series.empty() || width <= 0 || height <= 0) return rows;

    size_t start = series.size() > (size_t)width ? series.size() - width : 0;
    std::vector<double> vals;
    for (size_t i = start; i < series.size(); i++) {
        vals.push_back(series[i].value);
    }

    double max_val = 0;
    for (double v : vals) if (v > max_val) max_val = v;
    if (max_val == 0) max_val = 1;

    for (size_t col = 0; col < vals.size() && col < (size_t)width; col++) {
        int h = (int)(vals[col] / max_val * height);
        if (h < 0) h = 0;
        if (h > height) h = height;

        for (int row = height - 1; row >= height - h; row--) {
            if (row >= 0 && row < height) {
                if (row == height - h) {
                    // Top of bar
                    size_t byte_col = col; // each column is 1 byte for ASCII
                    rows[row][byte_col] = '*';
                } else {
                    rows[row][col] = '|';
                }
            }
        }
    }

    // Colorize
    std::vector<std::string> colored;
    for (int r = 0; r < height; r++) {
        std::string line;
        for (int c = 0; c < width; c++) {
            if (rows[r][c] == '*') line += std::string(color) + "*" + ansi::RESET;
            else if (rows[r][c] == '|') line += std::string(ansi::DIM) + "|" + ansi::RESET;
            else line += ' ';
        }
        colored.push_back(line);
    }
    return colored;
}

// ============================================================================
// TUI Panel
// ============================================================================

class TUIPanel {
private:
    KeyManager& key_manager;
    StatsCollector& stats;
    std::atomic<bool>& shutdown;
    std::thread render_thread;
    int scroll_offset = 0;
    std::function<void()> stop_callback;

    // Log buffer (captured from logger)
    std::mutex log_mtx;
    std::deque<std::string> log_lines;
    static constexpr size_t MAX_LOG_LINES = 200;

    int selected_tab = 0;  // 0=overview, 1=keys, 2=logs, 3=providers
    int current_max_w = 80;

    // Providers tab state (simulated multi-provider management)
    struct ProviderConfig {
        std::string name;
        std::string type;       // "nvidia", "openai", "anthropic", "groq", etc.
        std::string base_url;
        std::string api_key_masked;
        bool enabled = true;
        int priority = 0;
        std::string status = "ready";  // ready, error, cooldown
    };

    std::vector<ProviderConfig> providers;
    int selected_provider_idx = 0;

    ProviderManager* provider_manager = nullptr;

    void add_log_line(const std::string& line) {
        std::string clean;
        clean.reserve(line.size());
        for (char c : line) {
            if (c == '\r' || c == '\n') continue;
            clean += c;
        }
        std::lock_guard<std::mutex> lock(log_mtx);
        log_lines.push_back(clean);
        while (log_lines.size() > MAX_LOG_LINES) log_lines.pop_front();
    }

public:
    TUIPanel(KeyManager& km, StatsCollector& st, std::atomic<bool>& sd, ProviderManager* pm = nullptr)
        : key_manager(km), stats(st), shutdown(sd), provider_manager(pm) {
        // Seed initial providers (NVIDIA is primary; others are placeholders/configurable in TUI)
        providers = {
            {"NVIDIA NIM", "nvidia", "https://integrate.api.nvidia.com/v1", "...masked", true, 0, "ready"},
            {"OpenAI", "openai", "https://api.openai.com/v1", "", false, 1, "disabled"},
            {"Anthropic", "anthropic", "https://api.anthropic.com", "", false, 2, "disabled"},
            {"Groq", "groq", "https://api.groq.com/openai/v1", "", false, 3, "disabled"},
        };

        // Sync initial state to backend if available
        sync_to_provider_manager();
    }

    // Allow external code (e.g. main) to get current provider list for backend sync
    std::vector<TUIBackendProvider> get_current_providers() const {
        std::vector<TUIBackendProvider> out;
        for (const auto& p : providers) {
            TUIBackendProvider up;
            up.name = p.name;
            up.type = p.type;
            up.base_url = p.base_url;
            up.enabled = p.enabled;
            up.priority = p.priority;
            up.status = p.status;
            up.api_key_masked = p.api_key_masked;
            out.push_back(up);
        }
        return out;
    }

    void sync_to_provider_manager() {
        if (!provider_manager) return;
        auto ui_list = get_current_providers();
        // We pass the original keys from key_manager via initialize path.
        // For now the ProviderManager will use the fallback keys it was given.
        provider_manager->set_providers(ui_list, {}, 60, 1800);  // keys will be managed separately for now
    }

    void push_log(const std::string& line) {
        add_log_line(line);
    }

    void set_stop_callback(std::function<void()> cb) {
        stop_callback = cb;
    }

    void start() {
        render_thread = std::thread([this]() { run(); });
    }

    void stop() {
        shutdown.store(true);
        if (render_thread.joinable()) render_thread.join();
        term::disable_raw_mode();
        printf("%s%s", ansi::SHOW_CURSOR, ansi::CLEAR_SCREEN);
        printf("%s", ansi::move(1, 1).c_str());
        fflush(stdout);
    }

    std::vector<std::string> get_log_snapshot() {
        std::lock_guard<std::mutex> lock(log_mtx);
        return std::vector<std::string>(log_lines.begin(), log_lines.end());
    }

private:
    void run() {
        term::enable_raw_mode();
        printf("%s%s", ansi::HIDE_CURSOR, ansi::CLEAR_SCREEN);
        fflush(stdout);

        while (!shutdown.load()) {
            // Handle input
            int key = term::read_key();
            if (key == 'q' || key == 'Q' || key == 3) { // q, Q, Ctrl-C
                shutdown.store(true);
                if (stop_callback) stop_callback();
                break;
            }
            if (key == '\t' || key == '1') selected_tab = 0;
            if (key == '2') selected_tab = 1;
            if (key == '3') selected_tab = 2;
            if (key == '4') selected_tab = 3;

            // Providers tab navigation & actions (only when on tab 3)
            if (selected_tab == 3) {
                if (key == 1001) { // UP
                    selected_provider_idx = (std::max)(0, selected_provider_idx - 1);
                }
                if (key == 1002) { // DOWN
                    selected_provider_idx = (std::min)((int)providers.size() - 1, selected_provider_idx + 1);
                }
                if (key == 'e' || key == 'E') {
                    if (!providers.empty() && selected_provider_idx < (int)providers.size()) {
                        providers[selected_provider_idx].enabled = !providers[selected_provider_idx].enabled;
                        providers[selected_provider_idx].status = providers[selected_provider_idx].enabled ? "ready" : "disabled";
                        sync_to_provider_manager();
                    }
                }
                if (key == 'p' || key == 'P') {
                    if (!providers.empty() && selected_provider_idx < (int)providers.size()) {
                        providers[selected_provider_idx].priority = (providers[selected_provider_idx].priority + 1) % 5;
                        sync_to_provider_manager();
                    }
                }
                if (key == 's' || key == 'S') {
                    if (!providers.empty() && selected_provider_idx < (int)providers.size()) {
                        auto& pr = providers[selected_provider_idx];
                        if (pr.status == "ready") pr.status = "cooldown";
                        else if (pr.status == "cooldown") pr.status = "error";
                        else pr.status = "ready";
                        sync_to_provider_manager();
                    }
                }
                if (key == 'r' || key == 'R') {
                    if (!providers.empty() && selected_provider_idx < (int)providers.size()) {
                        auto& pr = providers[selected_provider_idx];
                        if (pr.name == "NVIDIA NIM") {
                            pr.enabled = true; pr.priority = 0; pr.status = "ready";
                        } else {
                            pr.enabled = false; pr.priority = selected_provider_idx; pr.status = "disabled";
                        }
                        sync_to_provider_manager();
                    }
                }
            } else {
                if (key == 1001) scroll_offset = (std::max)(0, scroll_offset - 1); // UP
                if (key == 1002) scroll_offset++; // DOWN
            }

            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        term::disable_raw_mode();
        printf("%s%s", ansi::SHOW_CURSOR, ansi::CLEAR_SCREEN);
        printf("%s", ansi::move(1, 1).c_str());
        fflush(stdout);
    }

    void emit(std::string& buf, int row, int col, const std::string& text) {
        buf += ansi::move(row, col);
        buf += text;
        size_t vis_len = visible_length(text);
        if (vis_len < (size_t)current_max_w) {
            buf += std::string(current_max_w - vis_len, ' ');
        }
    }

    void emit(int row, int col, const std::string& text) {
        printf("%s%s%s", ansi::move(row, col).c_str(), ansi::CLEAR_EOL, text.c_str());
    }

    void render() {
        int W = term::get_width();
        int H = term::get_height();
        int max_w = (std::max)(10, W - 1);
        int max_h = (std::max)(5, H - 1);
        current_max_w = max_w;
        int row = 1;

        auto snap = stats.snapshot();
        auto keys = key_manager.snapshot();

        std::string buf;
        buf.reserve(8192);
        buf += ansi::move(1, 1);

        // ─── Header ─────────────────────────────────────────────────
        std::string title = " NVIDIA NIM Load Balancer ";
        int title_pad = (max_w - (int)title.size()) / 2;
        if (title_pad < 1) title_pad = 1;

        emit(buf, row++, 1, std::string(ansi::BG_BLUE) + std::string(ansi::BOLD) + std::string(ansi::BRIGHT_WHITE)
            + std::string(max_w, ' ') + ansi::RESET);
        emit(buf, row++, 1, std::string(ansi::BG_BLUE) + std::string(ansi::BOLD) + std::string(ansi::BRIGHT_WHITE)
            + std::string(title_pad, ' ') + title + std::string(max_w - title_pad - (int)title.size(), ' ')
            + ansi::RESET);
        emit(buf, row++, 1, std::string(ansi::BG_BLUE) + std::string(ansi::BOLD) + std::string(ansi::BRIGHT_WHITE)
            + std::string(max_w, ' ') + ansi::RESET);

        // Status bar (high contrast WCAG AAA)
        std::string uptime_val = format_duration(snap.uptime_seconds);
        std::string keys_val = std::to_string(keys.size());
        std::string streams_val = std::to_string(snap.active_streams);
        std::string addr_val = "http://127.0.0.1:8100";

        int visible_len = 9 /*" RUNNING "*/
            + 9 /*" Uptime: "*/ + (int)uptime_val.size()
            + 8 /*"  Keys: "*/ + (int)keys_val.size()
            + 11 /*"  Streams: "*/ + (int)streams_val.size()
            + 2 /*"  "*/ + (int)addr_val.size() + 1 /*" "*/;
        int status_pad = max_w - visible_len;
        if (status_pad < 0) status_pad = 0;

        std::string status_line = std::string(ansi::BG_GREEN) + std::string(ansi::BOLD) + std::string(ansi::BRIGHT_WHITE)
            + " RUNNING " + ansi::RESET
            + std::string(ansi::BG_GRAY) + std::string(ansi::BOLD)
            + std::string(ansi::BRIGHT_WHITE) + " Uptime: "
            + std::string(ansi::BRIGHT_YELLOW) + uptime_val
            + std::string(ansi::BRIGHT_WHITE) + "  Keys: "
            + std::string(ansi::BRIGHT_YELLOW) + keys_val
            + std::string(ansi::BRIGHT_WHITE) + "  Streams: "
            + std::string(ansi::BRIGHT_YELLOW) + streams_val
            + std::string(ansi::BRIGHT_WHITE) + "  "
            + std::string(ansi::BRIGHT_CYAN) + addr_val
            + " "
            + std::string(status_pad, ' ')
            + ansi::RESET;
        emit(buf, row++, 1, status_line);
        emit(buf, row++, 1, "");

        // Tab bar
        const char* tab_names[] = {"[1] Overview", "[2] Keys", "[3] Logs", "[4] Providers"};
        std::string tabs;
        for (int i = 0; i < 4; i++) {
            if (i == selected_tab) {
                tabs += std::string(ansi::BOLD) + std::string(ansi::BG_CYAN) + std::string(ansi::WHITE) + " " + tab_names[i] + " " + ansi::RESET + " ";
            } else {
                tabs += std::string(ansi::DIM) + " " + tab_names[i] + " " + ansi::RESET + " ";
            }
        }
        std::string controls = "  [Tab] switch  [q] quit";
        if (selected_tab == 2) {
            controls += "  [Up/Down] scroll";
        } else if (selected_tab == 3) {
            controls += "  [↑/↓] select  [E/P/S/R] actions";
        }
        tabs += std::string(ansi::DIM) + controls + ansi::RESET;
        emit(buf, row++, 1, tabs);

        // Separator
        emit(buf, row++, 1, std::string(ansi::DIM) + std::string(max_w, '-') + ansi::RESET);
        emit(buf, row++, 1, "");

        // ─── Tab Content ────────────────────────────────────────────
        if (selected_tab == 0) {
            row = render_overview(buf, row, max_w, max_h, snap, keys);
        } else if (selected_tab == 1) {
            row = render_keys_detail(buf, row, max_w, max_h, snap, keys);
        } else if (selected_tab == 2) {
            row = render_logs(buf, row, max_w, max_h);
        } else if (selected_tab == 3) {
            row = render_providers(buf, row, max_w, max_h);
        }

        for (int r = row; r <= max_h; r++) {
            buf += ansi::move(r, 1);
            buf += std::string(max_w, ' ');
        }
        buf += ansi::move(row, 1);
        buf += ansi::CLEAR_EOS;

        fwrite(buf.data(), 1, buf.size(), stdout);
        fflush(stdout);
    }

    int render_overview(std::string& buf, int row, int max_w, int max_h,
                        const StatsSnapshot& snap,
                        const std::vector<KeySnapshot>& keys) {
        // ─── Traffic Statistics ───
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + ">> Traffic Statistics" + ansi::RESET);
            emit(buf, row++, 1, "");
        }

        auto stat_line = [&](const std::string& label, const std::string& value, const char* color) {
            if (row >= max_h) return;
            std::string padded_label = pad_right(label, 20);
            emit(buf, row, 1, std::string("  ") + std::string(ansi::DIM) + padded_label + ansi::RESET
                + std::string(color) + std::string(ansi::BOLD) + value + ansi::RESET);
            row++;
        };

        stat_line("Total Requests:", format_number(snap.total_requests), ansi::BRIGHT_CYAN);
        stat_line("Successes:", format_number(snap.total_successes), ansi::BRIGHT_GREEN);
        stat_line("Failures:", format_number(snap.total_failures),
                  snap.total_failures > 0 ? ansi::BRIGHT_RED : ansi::DIM);
        stat_line("Rate Limits (429):", format_number(snap.total_rate_limits),
                  snap.total_rate_limits > 0 ? ansi::BRIGHT_YELLOW : ansi::DIM);
        stat_line("Success Rate:", format_number(snap.success_rate) + "%",
                  snap.success_rate >= 95 ? ansi::BRIGHT_GREEN :
                  snap.success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED);
        stat_line("Requests/min:", format_number(snap.requests_per_minute), ansi::BRIGHT_CYAN);
        stat_line("Active Streams:", std::to_string(snap.active_streams), ansi::BRIGHT_BLUE);

        emit(buf, row++, 1, "");

        // ─── Latency ───
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + ">> Latency" + ansi::RESET);
            emit(buf, row++, 1, "");
            stat_line("Average:", format_number(snap.avg_latency_ms) + " ms", ansi::BRIGHT_CYAN);
            stat_line("P95:", format_number(snap.p95_latency_ms) + " ms",
                      snap.p95_latency_ms < 2000 ? ansi::BRIGHT_GREEN : ansi::BRIGHT_YELLOW);
            stat_line("P99:", format_number(snap.p99_latency_ms) + " ms",
                      snap.p99_latency_ms < 5000 ? ansi::BRIGHT_GREEN : ansi::BRIGHT_YELLOW);
            stat_line("Min / Max:", format_number(snap.min_latency_ms) + " / "
                      + format_number(snap.max_latency_ms) + " ms", ansi::WHITE);
        }

        emit(buf, row++, 1, "");

        // ─── Key Health ───
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + ">> Key Health" + ansi::RESET);
            emit(buf, row++, 1, "");
        }

        // Table header
        int key_name_w = (std::min)(15, max_w - 50);
        std::string hdr = "  " + pad_right("Key", key_name_w) + " "
            + pad_left("State", 9) + " "
            + pad_left("Reqs", 6) + " "
            + pad_left("OK", 5) + " "
            + pad_left("Fail", 5) + " "
            + pad_left("CD", 4);
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::DIM) + hdr + ansi::RESET);
        }

        for (const auto& k : keys) {
            if (row >= max_h - 10) break;
            std::string state_str = (k.state == "available") ? "ready" : "cooldown";
            std::string state_color = (k.state == "available") ? ansi::GREEN : ansi::RED;
            std::string cd_str = k.cooldown_remaining_sec > 0 ? std::to_string(k.cooldown_remaining_sec) + "s" : "-";
            std::string fail_color = k.total_failures > 0 ? ansi::RED : ansi::DIM;

            std::string line = "  " + pad_right(k.masked, key_name_w) + " "
                + state_color + pad_left(state_str, 9) + ansi::RESET + " "
                + pad_left(std::to_string(k.total_requests), 6) + " "
                + ansi::GREEN + pad_left(std::to_string(k.total_successes), 5) + ansi::RESET + " "
                + fail_color + pad_left(std::to_string(k.total_failures), 5) + ansi::RESET + " "
                + pad_left(cd_str, 4);
            emit(buf, row++, 1, line);
        }

        emit(buf, row++, 1, "");

        // ─── Throughput Chart (dynamically sized to fill remaining screen down to bottom row max_h) ───
        int chart_w = (std::max)(20, max_w - 4);
        if (row < max_h - 1) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + ">> Throughput (reqs/5s)" + ansi::RESET);
        }
        int chart_h = (std::max)(3, max_h - row);
        auto chart = area_chart(snap.throughput_series, chart_w, chart_h, ansi::BRIGHT_CYAN, ansi::CYAN);
        for (const auto& line : chart) {
            if (row >= max_h) break;
            emit(buf, row++, 1, "  " + line);
        }

        // ─── Footer Sparklines (anchored to bottom row max_h) ───
        int avail = max_w - 26;
        if (avail < 10) avail = 10;
        int tp_w = (std::max)(6, avail * 3 / 5);
        int err_w = (std::max)(4, avail - tp_w);
        if (24 + tp_w + err_w > max_w - 2) {
            int excess = (24 + tp_w + err_w) - (max_w - 2);
            err_w = (std::max)(4, err_w - excess);
            if (24 + tp_w + err_w > max_w - 2) {
                tp_w = (std::max)(6, max_w - 28);
                err_w = 4;
            }
        }
        emit(buf, max_h, 1, std::string(ansi::DIM) + " Throughput: " + ansi::RESET
            + sparkline(snap.throughput_series, tp_w, ansi::BRIGHT_CYAN)
            + " "
            + std::string(ansi::DIM) + "  Errors: " + ansi::RESET
            + sparkline(snap.error_rate_series, err_w, ansi::BRIGHT_RED));

        return max_h + 1;
    }

    int render_keys_detail(std::string& buf, int row, int max_w, int max_h,
                           const StatsSnapshot& snap,
                           const std::vector<KeySnapshot>& keys) {
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN)
                + ">> Key Pool Details" + ansi::RESET);
            emit(buf, row++, 1, "");
        }

        for (size_t i = 0; i < keys.size(); i++) {
            if (row >= max_h - 4) break;
            const auto& k = keys[i];

            std::string state_badge;
            if (k.state == "available") {
                state_badge = std::string(ansi::BG_GREEN) + std::string(ansi::BOLD) + std::string(ansi::WHITE) + " AVAILABLE " + ansi::RESET;
            } else {
                state_badge = std::string(ansi::BG_RED) + std::string(ansi::BOLD) + std::string(ansi::WHITE) + " COOLDOWN " + ansi::RESET;
            }

            emit(buf, row++, 1, std::string(ansi::BOLD) + "  Key #" + std::to_string(i + 1)
                + " " + k.masked + ansi::RESET + "  " + state_badge);

            double success_rate = k.total_requests > 0 ? (double)k.total_successes / k.total_requests * 100.0 : 0;
            std::string sr_color = success_rate >= 95 ? ansi::BRIGHT_GREEN :
                                   success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED;

            int bar_w = (std::max)(10, max_w - 40);
            std::string health_bar = bar(success_rate, 100.0, bar_w,
                                         success_rate >= 95 ? ansi::BRIGHT_GREEN :
                                         success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED);

            emit(buf, row++, 1, "    Success Rate: " + health_bar + " " + sr_color
                + format_number(success_rate) + "%" + ansi::RESET);

            emit(buf, row++, 1, std::string(ansi::DIM) + "    Requests: " + std::to_string(k.total_requests)
                + "  |  Successes: " + std::to_string(k.total_successes)
                + "  |  Failures: " + std::to_string(k.total_failures)
                + "  |  Consecutive: " + std::to_string(k.consecutive_failures)
                + "  |  Cooldown: " + (k.cooldown_remaining_sec > 0 ? std::to_string(k.cooldown_remaining_sec) + "s" : "none")
                + ansi::RESET);

            emit(buf, row++, 1, "");
        }

        if (keys.empty() && row < max_h) {
            emit(buf, row++, 1, std::string(ansi::YELLOW) + "  No API keys configured." + ansi::RESET);
        }

        return row;
    }

    int render_logs(std::string& buf, int row, int max_w, int max_h) {
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN)
                + ">> Activity Log" + ansi::RESET);
            emit(buf, row++, 1, "");
        }

        auto logs = get_log_snapshot();
        int visible = max_h - row;
        if (visible < 1) visible = 1;

        int start = (int)logs.size() - visible - scroll_offset;
        if (start < 0) start = 0;
        int end = start + visible;
        if (end > (int)logs.size()) end = (int)logs.size();

        for (int i = start; i < end; i++) {
            if (row >= max_h) break;
            std::string line = logs[i];
            emit(buf, row++, 1, "  " + truncate_ansi(line, max_w - 4));
        }

        if (logs.empty() && row < max_h) {
            emit(buf, row++, 1, std::string(ansi::DIM) + "  No log entries yet..." + ansi::RESET);
        }

        return row;
    }

    int render_providers(std::string& buf, int row, int max_w, int max_h) {
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN)
                + ">> AI Providers (multi-backend configuration)" + ansi::RESET);
            emit(buf, row++, 1, std::string(ansi::DIM) + "   Select provider and toggle/configure routing priority" + ansi::RESET);
            emit(buf, row++, 1, "");
        }

        // Header
        std::string hdr = "  " + pad_right("Provider", 18) + " "
            + pad_right("Type", 12) + " "
            + pad_right("Base URL", 28) + " "
            + pad_left("Enabled", 9) + " "
            + pad_left("Pri", 4) + " "
            + pad_left("Status", 10);
        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::DIM) + hdr + ansi::RESET);
            emit(buf, row++, 1, std::string(ansi::DIM) + std::string(max_w - 2, '-') + ansi::RESET);
        }

        for (size_t i = 0; i < providers.size(); ++i) {
            if (row >= max_h - 1) break;

            const auto& p = providers[i];
            bool is_selected = ((int)i == selected_provider_idx);

            std::string sel_prefix = is_selected ? std::string(ansi::BG_CYAN) + std::string(ansi::BLACK) + "> " + ansi::RESET : "  ";

            std::string en_badge = p.enabled ?
                (std::string(ansi::BG_GREEN) + std::string(ansi::WHITE) + "  ON  " + ansi::RESET) :
                (std::string(ansi::BG_GRAY)  + std::string(ansi::WHITE) + " OFF  " + ansi::RESET);

            std::string st_color;
            if (p.status == "ready") st_color = ansi::BRIGHT_GREEN;
            else if (p.status == "cooldown") st_color = ansi::YELLOW;
            else if (p.status == "error") st_color = ansi::BRIGHT_RED;
            else st_color = ansi::DIM;

            std::string url_short = truncate(p.base_url, 27);

            std::string line = sel_prefix
                + pad_right(p.name, 18) + " "
                + pad_right(p.type, 12) + " "
                + pad_right(url_short, 28) + " "
                + en_badge + " "
                + pad_left(std::to_string(p.priority), 4) + " "
                + st_color + pad_left(p.status, 10) + ansi::RESET;

            emit(buf, row++, 1, line);
        }

        emit(buf, row++, 1, "");

        // Selected provider details + help
        if (!providers.empty() && selected_provider_idx < (int)providers.size() && row < max_h) {
            const auto& p = providers[selected_provider_idx];

            emit(buf, row++, 1, std::string(ansi::BOLD) + "  Selected: " + ansi::RESET
                + std::string(ansi::BRIGHT_CYAN) + p.name + ansi::RESET
                + "  (" + p.type + ")");

            std::string api_key_display = p.api_key_masked.empty() ? "<not set>" : p.api_key_masked;
            emit(buf, row++, 1, std::string(ansi::DIM) + "    Base URL: " + ansi::RESET + p.base_url);
            emit(buf, row++, 1, std::string(ansi::DIM) + "    API Key:  " + ansi::RESET + api_key_display);
            emit(buf, row++, 1, std::string(ansi::DIM) + "    Priority: " + ansi::RESET + std::to_string(p.priority)
                + "   (lower = higher precedence when routing)");

            emit(buf, row++, 1, "");
        }

        if (row < max_h) {
            emit(buf, row++, 1, std::string(ansi::DIM) + "  Note: This TUI simulates provider config. Real multi-provider routing is not yet wired into backend." + ansi::RESET);
        }

        return row;
    }
};

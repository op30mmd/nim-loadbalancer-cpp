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
    }
}

inline void enable_raw_mode() {
    if (!raw_mode_active) {
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
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(h, &mode);
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(h, mode);
    // Enable virtual terminal processing for ANSI codes
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode;
    GetConsoleMode(out, &out_mode);
    out_mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    SetConsoleMode(out, out_mode);
}

inline void disable_raw_mode() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(h, &mode);
    mode |= (ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(h, mode);
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

    // Colors
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

    // Background
    const char* const BG_RED    = "\033[41m";
    const char* const BG_GREEN  = "\033[42m";
    const char* const BG_YELLOW = "\033[43m";
    const char* const BG_BLUE   = "\033[44m";
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

inline std::string pad_right(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

inline std::string pad_left(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return std::string(width - s.size(), ' ') + s;
}

// Draw a horizontal bar chart using Unicode block characters
inline std::string bar(double value, double max_val, int width,
                       const char* color = ansi::BRIGHT_CYAN) {
    if (max_val <= 0 || value <= 0 || width <= 0) return std::string(width, ' ');
    double ratio = std::min(value / max_val, 1.0);
    int filled = (int)(ratio * width);
    int partial = (int)((ratio * width - filled) * 8);

    static const char* blocks[] = {" ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
                                    "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"};

    std::string result = std::string(color);
    for (int i = 0; i < filled && i < width; i++) result += "\xe2\x96\x88"; // full block
    if (filled < width && partial > 0) {
        result += blocks[partial];
        filled++;
    }
    result += ansi::RESET;
    if (filled < width) result += std::string(width - filled, ' ');
    return result;
}

// Draw a sparkline from a time series
inline std::string sparkline(const std::vector<TimeSeriesPoint>& series, int width,
                             const char* color = ansi::BRIGHT_CYAN) {
    static const char* chars[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };

    if (series.empty() || width <= 0) return std::string(width, ' ');

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
        int idx = (int)(vals[i] / max_val * 7.0);
        if (idx < 0) idx = 0;
        if (idx > 7) idx = 7;
        result += chars[idx];
    }
    result += ansi::RESET;
    if (vals.size() < (size_t)width) {
        result += std::string(width - vals.size(), ' ');
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

    int selected_tab = 0;  // 0=overview, 1=keys, 2=logs

    void add_log_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mtx);
        log_lines.push_back(line);
        while (log_lines.size() > MAX_LOG_LINES) log_lines.pop_front();
    }

public:
    TUIPanel(KeyManager& km, StatsCollector& st, std::atomic<bool>& sd)
        : key_manager(km), stats(st), shutdown(sd) {}

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
        printf("%s", ansi::move(1, 1));
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
            if (key == 1001) scroll_offset = std::max(0, scroll_offset - 1); // UP
            if (key == 1002) scroll_offset++; // DOWN

            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        term::disable_raw_mode();
        printf("%s%s", ansi::SHOW_CURSOR, ansi::CLEAR_SCREEN);
        printf("%s", ansi::move(1, 1));
        fflush(stdout);
    }

    void emit(int row, int col, const std::string& text) {
        printf("%s%s%s", ansi::move(row, col).c_str(), ansi::CLEAR_EOL, text.c_str());
    }

    void render() {
        int W = term::get_width();
        int H = term::get_height();
        int row = 1;

        auto snap = stats.snapshot();
        auto keys = key_manager.snapshot();

        // ─── Header ─────────────────────────────────────────────────
        std::string title = " NVIDIA NIM Load Balancer ";
        int title_pad = (W - (int)title.size()) / 2;
        if (title_pad < 1) title_pad = 1;

        emit(row++, 1, std::string(ansi::BG_BLUE) + std::string(ansi::BOLD) + std::string(ansi::WHITE)
            + std::string(title_pad, ' ') + title + std::string(title_pad, ' ')
            + ansi::RESET);

        // Status bar
        std::string status_color = ansi::BG_GREEN;
        std::string status_text = " RUNNING ";
        std::string uptime_str = " Uptime: " + format_duration(snap.uptime_seconds) + " ";
        std::string keys_str = " Keys: " + std::to_string(keys.size()) + " ";
        std::string streams_str = " Streams: " + std::to_string(snap.active_streams) + " ";
        std::string addr_str = " http://127.0.0.1:8100 ";

        emit(row++, 1, std::string(status_color) + std::string(ansi::BOLD) + std::string(ansi::WHITE)
            + status_text + ansi::RESET
            + std::string(ansi::BG_GRAY) + std::string(ansi::WHITE)
            + uptime_str + keys_str + streams_str + addr_str
            + ansi::RESET);

        // Tab bar
        const char* tab_names[] = {"[1] Overview", "[2] Keys", "[3] Logs"};
        std::string tabs;
        for (int i = 0; i < 3; i++) {
            if (i == selected_tab) {
                tabs += std::string(ansi::BOLD) + std::string(ansi::BRIGHT_CYAN) + " " + tab_names[i] + " " + ansi::RESET;
            } else {
                tabs += std::string(ansi::DIM) + " " + tab_names[i] + " " + ansi::RESET;
            }
        }
        tabs += std::string(ansi::DIM) + "  [Tab] switch  [q] quit" + ansi::RESET;
        emit(row++, 1, tabs);

        // Separator
        emit(row++, 1, std::string(ansi::DIM) + std::string(W - 1, '\xe2\x94\x80') + ansi::RESET);

        // ─── Tab Content ────────────────────────────────────────────
        if (selected_tab == 0) {
            row = render_overview(row, W, H, snap, keys);
        } else if (selected_tab == 1) {
            row = render_keys_detail(row, W, H, snap, keys);
        } else {
            row = render_logs(row, W, H);
        }
        fflush(stdout);
    }

    int render_overview(int row, int W, int H, const StatsSnapshot& snap,
                        const std::vector<KeySnapshot>& keys) {
        int col_w = W / 2;
        int left_col = 1;
        int right_col = col_w + 2;
        int right_row_start = row;  // Align right column with left column start

        // ─── Stats Cards (left column) ───
        emit(row, left_col, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + "Traffic Statistics" + ansi::RESET);
        row++;

        auto stat_line = [&](const std::string& label, const std::string& value, const char* color) {
            std::string padded_label = pad_right(label, 20);
            emit(row, left_col, std::string("  ") + std::string(ansi::DIM) + padded_label + ansi::RESET
                + std::string(color) + std::string(ansi::BOLD) + value + ansi::RESET);
            row++;
        };

        stat_line("Total Requests:", format_number(snap.total_requests), ansi::BRIGHT_CYAN);
        stat_line("Successes:", format_number(snap.total_successes), ansi::BRIGHT_GREEN);
        stat_line("Failures:", format_number(snap.total_failures),
                  snap.total_failures > 0 ? ansi::BRIGHT_RED : ansi::DIM);
        stat_line("Rate Limits (429):", format_number(snap.total_rate_limits),
                  snap.total_rate_limits > 0 ? ansi::BRIGHT_YELLOW : ansi::DIM);
        stat_line("Auth Failures:", format_number(snap.total_auth_failures),
                  snap.total_auth_failures > 0 ? ansi::BRIGHT_RED : ansi::DIM);
        stat_line("Server Errors:", format_number(snap.total_server_errors),
                  snap.total_server_errors > 0 ? ansi::BRIGHT_RED : ansi::DIM);
        stat_line("Success Rate:", format_number(snap.success_rate) + "%",
                  snap.success_rate >= 95 ? ansi::BRIGHT_GREEN :
                  snap.success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED);
        stat_line("Requests/min:", format_number(snap.requests_per_minute), ansi::BRIGHT_CYAN);
        stat_line("Active Streams:", std::to_string(snap.active_streams), ansi::BRIGHT_BLUE);

        row++;

        // ─── Latency (left column continued) ───
        emit(row, left_col, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + "Latency" + ansi::RESET);
        row++;
        stat_line("Average:", format_number(snap.avg_latency_ms) + " ms", ansi::BRIGHT_CYAN);
        stat_line("P95:", format_number(snap.p95_latency_ms) + " ms",
                  snap.p95_latency_ms < 2000 ? ansi::BRIGHT_GREEN : ansi::BRIGHT_YELLOW);
        stat_line("P99:", format_number(snap.p99_latency_ms) + " ms",
                  snap.p99_latency_ms < 5000 ? ansi::BRIGHT_GREEN : ansi::BRIGHT_YELLOW);
        stat_line("Min / Max:", format_number(snap.min_latency_ms) + " / "
                  + format_number(snap.max_latency_ms) + " ms", ansi::WHITE);

        row++;

        // ─── Key Summary (right column) ───
        int rrow = right_row_start;

        emit(rrow, right_col, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + "Key Health" + ansi::RESET);
        rrow++;

        // Table header
        int key_name_w = std::min(12, col_w - 30);
        std::string hdr = "  " + pad_right("Key", key_name_w) + " "
            + pad_left("State", 9) + " "
            + pad_left("Reqs", 6) + " "
            + pad_left("OK", 5) + " "
            + pad_left("Fail", 5) + " "
            + pad_left("CD", 4);
        emit(rrow++, right_col, std::string(ansi::DIM) + hdr + ansi::RESET);

        for (const auto& k : keys) {
            if (rrow >= H - 1) break;
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
            emit(rrow++, right_col, line);
        }

        rrow += 1;

        // ─── Throughput Chart (right column) ───
        int chart_w = std::max(20, col_w - 4);
        emit(rrow++, right_col, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + "Throughput (reqs/5s)" + ansi::RESET);
        auto chart = area_chart(snap.throughput_series, chart_w, 5, ansi::BRIGHT_CYAN, ansi::CYAN);
        for (const auto& line : chart) {
            if (rrow >= H - 1) break;
            emit(rrow++, right_col, "  " + line);
        }
        rrow++;

        // ─── Latency Chart (right column) ───
        emit(rrow++, right_col, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN) + "Latency (ms avg/5s)" + ansi::RESET);
        auto lchart = area_chart(snap.latency_series, chart_w, 5, ansi::BRIGHT_YELLOW, ansi::YELLOW);
        for (const auto& line : lchart) {
            if (rrow >= H - 1) break;
            emit(rrow++, right_col, "  " + line);
        }

        // ─── Bottom: Sparklines ───
        row = H - 3;
        int spark_w = std::max(20, W - 30);
        emit(row++, 1, std::string(ansi::DIM) + " Throughput: " + ansi::RESET
            + sparkline(snap.throughput_series, spark_w, ansi::BRIGHT_CYAN)
            + std::string(ansi::DIM) + "  Errors: " + ansi::RESET
            + sparkline(snap.error_rate_series, std::max(10, spark_w / 2), ansi::BRIGHT_RED));
        emit(row++, 1, std::string(ansi::DIM) + " [1]Overview [2]Keys [3]Logs  [q]Quit  [Tab]Next" + ansi::RESET);

        return row;
    }

    int render_keys_detail(int row, int W, int H, const StatsSnapshot& snap,
                           const std::vector<KeySnapshot>& keys) {
        emit(row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN)
            + " Key Pool Details" + ansi::RESET);
        row++;

        for (size_t i = 0; i < keys.size(); i++) {
            if (row >= H - 4) break;
            const auto& k = keys[i];

            std::string state_badge;
            if (k.state == "available") {
                state_badge = std::string(ansi::BG_GREEN) + std::string(ansi::BOLD) + std::string(ansi::WHITE) + " AVAILABLE " + ansi::RESET;
            } else {
                state_badge = std::string(ansi::BG_RED) + std::string(ansi::BOLD) + std::string(ansi::WHITE) + " COOLDOWN " + ansi::RESET;
            }

            emit(row++, 1, std::string(ansi::BOLD) + "  Key #" + std::to_string(i + 1)
                + " " + k.masked + ansi::RESET + "  " + state_badge);

            double success_rate = k.total_requests > 0 ? (double)k.total_successes / k.total_requests * 100.0 : 0;
            std::string sr_color = success_rate >= 95 ? ansi::BRIGHT_GREEN :
                                   success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED;

            int bar_w = std::max(10, W - 40);
            std::string health_bar = bar(success_rate, 100.0, bar_w,
                                         success_rate >= 95 ? ansi::BRIGHT_GREEN :
                                         success_rate >= 80 ? ansi::BRIGHT_YELLOW : ansi::BRIGHT_RED);

            emit(row++, 1, "    Success Rate: " + health_bar + " " + sr_color
                + format_number(success_rate) + "%" + ansi::RESET);

            emit(row++, 1, std::string(ansi::DIM) + "    Requests: " + std::to_string(k.total_requests)
                + "  |  Successes: " + std::to_string(k.total_successes)
                + "  |  Failures: " + std::to_string(k.total_failures)
                + "  |  Consecutive: " + std::to_string(k.consecutive_failures)
                + "  |  Cooldown: " + (k.cooldown_remaining_sec > 0 ? std::to_string(k.cooldown_remaining_sec) + "s" : "none")
                + ansi::RESET);

            row++;
        }

        if (keys.empty()) {
            emit(row++, 1, std::string(ansi::YELLOW) + "  No API keys configured." + ansi::RESET);
        }

        // Footer
        row = H - 2;
        emit(row++, 1, std::string(ansi::DIM) + " [1]Overview [2]Keys [3]Logs  [q]Quit  [Tab]Next" + ansi::RESET);

        return row;
    }

    int render_logs(int row, int W, int H) {
        emit(row++, 1, std::string(ansi::BOLD) + std::string(ansi::BRIGHT_GREEN)
            + " Activity Log" + ansi::RESET);
        row++;

        auto logs = get_log_snapshot();
        int visible = H - row - 2;
        if (visible < 1) visible = 1;

        int start = (int)logs.size() - visible - scroll_offset;
        if (start < 0) start = 0;
        int end = start + visible;
        if (end > (int)logs.size()) end = (int)logs.size();

        for (int i = start; i < end; i++) {
            if (row >= H - 2) break;
            std::string line = logs[i];
            // Truncate to terminal width
            // Strip ANSI codes for length calculation
            size_t visible_len = 0;
            bool in_escape = false;
            for (char c : line) {
                if (c == '\033') in_escape = true;
                else if (in_escape && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) in_escape = false;
                else if (!in_escape) visible_len++;
            }

            emit(row++, 1, "  " + truncate(line, W - 4));
        }

        if (logs.empty()) {
            emit(row++, 1, std::string(ansi::DIM) + "  No log entries yet..." + ansi::RESET);
        }

        row = H - 2;
        emit(row++, 1, std::string(ansi::DIM) + " [1]Overview [2]Keys [3]Logs  [q]Quit  [Up/Down]Scroll" + ansi::RESET);

        return row;
    }
};

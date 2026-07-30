#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cmath>

// ============================================================================
// Statistics Collector
// ============================================================================

struct TimeSeriesPoint {
    std::chrono::steady_clock::time_point timestamp;
    double value;
};

struct RequestRecord {
    std::chrono::steady_clock::time_point timestamp;
    int status_code;
    long long latency_ms;
    std::string endpoint;
    bool success;
};

struct StatsSnapshot {
    // Totals
    int total_requests = 0;
    int total_successes = 0;
    int total_failures = 0;
    int total_rate_limits = 0;
    int total_auth_failures = 0;
    int total_server_errors = 0;
    int active_streams = 0;

    // Rates
    double requests_per_minute = 0.0;
    double success_rate = 0.0;

    // Latency
    double avg_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
    double p99_latency_ms = 0.0;
    double min_latency_ms = 0.0;
    double max_latency_ms = 0.0;

    // Time series (last N buckets)
    std::vector<TimeSeriesPoint> throughput_series;   // requests per bucket
    std::vector<TimeSeriesPoint> latency_series;      // avg latency per bucket
    std::vector<TimeSeriesPoint> error_rate_series;   // errors per bucket

    // Recent activity
    std::vector<RequestRecord> recent_requests;

    // Uptime
    long long uptime_seconds = 0;
};

class StatsCollector {
private:
    mutable std::mutex mtx;
    std::chrono::steady_clock::time_point start_time;

    // Rolling windows
    static constexpr size_t MAX_RECENT = 100;
    static constexpr size_t MAX_SERIES = 60;  // 60 buckets
    static constexpr double BUCKET_SIZE_SEC = 5.0;  // 5-second buckets

    std::deque<RequestRecord> recent;
    std::deque<TimeSeriesPoint> throughput_raw;
    std::deque<TimeSeriesPoint> latency_raw;
    std::deque<TimeSeriesPoint> error_raw;

    // Counters
    std::atomic<int> total_requests{0};
    std::atomic<int> total_successes{0};
    std::atomic<int> total_failures{0};
    std::atomic<int> total_rate_limits{0};
    std::atomic<int> total_auth_failures{0};
    std::atomic<int> total_server_errors{0};
    std::atomic<int> active_streams{0};

    // Current bucket accumulators
    double bucket_start_sec = 0.0;
    int bucket_requests = 0;
    double bucket_latency_sum = 0.0;
    int bucket_latency_count = 0;
    int bucket_errors = 0;

    double elapsed_sec() const {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
    }

    void maybe_flush_bucket() {
        double now_sec = elapsed_sec();
        if (now_sec - bucket_start_sec >= BUCKET_SIZE_SEC) {
            auto ts = std::chrono::steady_clock::now();
            if (bucket_latency_count > 0) {
                throughput_raw.push_back({ts, (double)bucket_requests});
                latency_raw.push_back({ts, bucket_latency_sum / bucket_latency_count});
                error_raw.push_back({ts, (double)bucket_errors});

                while (throughput_raw.size() > MAX_SERIES) throughput_raw.pop_front();
                while (latency_raw.size() > MAX_SERIES) latency_raw.pop_front();
                while (error_raw.size() > MAX_SERIES) error_raw.pop_front();
            } else if (bucket_requests > 0) {
                throughput_raw.push_back({ts, (double)bucket_requests});
                error_raw.push_back({ts, (double)bucket_errors});
                while (throughput_raw.size() > MAX_SERIES) throughput_raw.pop_front();
                while (error_raw.size() > MAX_SERIES) error_raw.pop_front();
            }
            bucket_requests = 0;
            bucket_latency_sum = 0.0;
            bucket_latency_count = 0;
            bucket_errors = 0;
            bucket_start_sec = now_sec;
        }
    }

    static double percentile(std::vector<double>& sorted_vals, double p) {
        if (sorted_vals.empty()) return 0.0;
        double idx = p * (sorted_vals.size() - 1);
        size_t lo = (size_t)std::floor(idx);
        size_t hi = (size_t)std::ceil(idx);
        if (lo == hi || hi >= sorted_vals.size()) return sorted_vals[lo];
        double frac = idx - lo;
        return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac;
    }

public:
    StatsCollector() {
        start_time = std::chrono::steady_clock::now();
        bucket_start_sec = 0.0;
    }

    void record_request(int status_code, long long latency_ms,
                        const std::string& endpoint, bool success) {
        total_requests.fetch_add(1, std::memory_order_relaxed);
        if (success) {
            total_successes.fetch_add(1, std::memory_order_relaxed);
        } else {
            total_failures.fetch_add(1, std::memory_order_relaxed);
            if (status_code == 429) total_rate_limits.fetch_add(1, std::memory_order_relaxed);
            if (status_code == 401 || status_code == 403) total_auth_failures.fetch_add(1, std::memory_order_relaxed);
            if (status_code >= 500) total_server_errors.fetch_add(1, std::memory_order_relaxed);
        }

        RequestRecord rec;
        rec.timestamp = std::chrono::steady_clock::now();
        rec.status_code = status_code;
        rec.latency_ms = latency_ms;
        rec.endpoint = endpoint;
        rec.success = success;

        std::lock_guard<std::mutex> lock(mtx);
        recent.push_front(rec);
        while (recent.size() > MAX_RECENT) recent.pop_back();

        bucket_requests++;
        if (latency_ms >= 0) {
            bucket_latency_sum += latency_ms;
            bucket_latency_count++;
        }
        if (!success) bucket_errors++;

        maybe_flush_bucket();
    }

    void stream_started() { active_streams.fetch_add(1, std::memory_order_relaxed); }
    void stream_ended() { active_streams.fetch_sub(1, std::memory_order_relaxed); }

    StatsSnapshot snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        maybe_flush_bucket();

        StatsSnapshot s;
        s.total_requests = total_requests.load(std::memory_order_relaxed);
        s.total_successes = total_successes.load(std::memory_order_relaxed);
        s.total_failures = total_failures.load(std::memory_order_relaxed);
        s.total_rate_limits = total_rate_limits.load(std::memory_order_relaxed);
        s.total_auth_failures = total_auth_failures.load(std::memory_order_relaxed);
        s.total_server_errors = total_server_errors.load(std::memory_order_relaxed);
        s.active_streams = active_streams.load(std::memory_order_relaxed);
        s.uptime_seconds = (long long)elapsed_sec();

        if (s.total_requests > 0) {
            s.success_rate = (double)s.total_successes / s.total_requests * 100.0;
        }

        // Compute RPM from recent requests in last 60 seconds
        auto now = std::chrono::steady_clock::now();
        int last_60s = 0;
        for (const auto& r : recent) {
            double age = std::chrono::duration<double>(now - r.timestamp).count();
            if (age <= 60.0) last_60s++;
            else break;
        }
        // Scale up if we haven't been running for 60s
        double window = std::min(elapsed_sec(), 60.0);
        if (window > 0) {
            s.requests_per_minute = (double)last_60s / window * 60.0;
        }

        // Latency stats from recent
        std::vector<double> latencies;
        for (const auto& r : recent) {
            if (r.latency_ms >= 0) latencies.push_back((double)r.latency_ms);
        }
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            double sum = 0;
            for (double v : latencies) sum += v;
            s.avg_latency_ms = sum / latencies.size();
            s.min_latency_ms = latencies.front();
            s.max_latency_ms = latencies.back();
            s.p95_latency_ms = percentile(latencies, 0.95);
            s.p99_latency_ms = percentile(latencies, 0.99);
        }

        // Copy time series
        s.throughput_series.assign(throughput_raw.begin(), throughput_raw.end());
        s.latency_series.assign(latency_raw.begin(), latency_raw.end());
        s.error_rate_series.assign(error_raw.begin(), error_raw.end());

        // Recent requests (already newest-first)
        s.recent_requests.assign(recent.begin(), recent.end());

        return s;
    }
};

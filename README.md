# NVIDIA NIM Load Balancer (C++)

A high-performance reverse proxy and load balancer for the [NVIDIA NIM](https://build.nvidia.com/) inference API. Provides multi-key rotation with adaptive backoff, Anthropic and OpenAI protocol compatibility, streaming support, and a built-in terminal UI control panel.

## Features

- **Multi-key rotation**: Round-robin across an arbitrary number of NVIDIA API keys with error-class-aware exponential backoff (auth, rate-limit, server, network)
- **Protocol translation**: Accepts both Anthropic Messages API and OpenAI Chat Completions requests, translates them to NIM's native format, and converts responses back
- **Streaming**: Full SSE streaming support for both protocols with proper event framing, mid-stream error handling, and client disconnect detection
- **Thinking/reasoning**: Translates Anthropic `thinking` blocks and OpenAI `reasoning_content` fields; auto-scales `max_tokens` when thinking is enabled
- **Model mapping**: Built-in mapping from Claude/GPT model names to NIM equivalents; overridable via `NIM_MODEL_MAP` environment variable
- **Model-specific overrides**: Automatic `chat_template_kwargs` injection for GLM-5, DeepSeek, Nemotron, Qwen, and other NIM models
- **Client-side rate limiting**: Configurable concurrency cap and minimum inter-request interval to stay within NIM free-tier quotas
- **TUI control panel**: Fullscreen terminal UI with live traffic statistics, latency percentiles, key health dashboard, throughput/latency charts, sparklines, and activity log
- **CORS**: Permissive CORS headers for browser-based clients

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Health check (`{"status":"ok"}`) |
| `GET` | `/v1/keys` | Key pool health (masked keys, state, cooldown) |
| `GET` | `/v1/models` | NIM model catalog (cached 1h) |
| `POST` | `/v1/messages` | Anthropic Messages API |
| `POST` | `/v1/chat/completions` | OpenAI Chat Completions |
| `POST` | `/v1/completions` | OpenAI Completions |
| `POST` | `/v1/embeddings` | OpenAI Embeddings |
| `POST` | `/v1/(.*)` | Wildcard passthrough to NIM |

All `/v1/*` routes also accept bare paths (`/messages`, `/models`).

## Build

### Prerequisites

- C++17 compiler (GCC 8+, Clang 7+, MSVC 2019+)
- CMake 3.15+
- On Linux: OpenSSL development headers (`libssl-dev` on Debian/Ubuntu)
- On Windows: No additional dependencies (uses Schannel)

### Building with CMake

```bash
# Clone with submodules
git clone --recurse-submodules --shallow-submodules https://github.com/your-repo/nim-loadbalancer-cpp.git
cd nim-loadbalancer-cpp

# Or if already cloned without submodules:
# git submodule update --init --recursive --depth 1

# Configure and build
cmake -B build
cmake --build build --config Release
```

The binary is produced as `build/nim-balancer` (or `build/Release/nim-balancer.exe` on Windows).

### Visual Studio

Open the generated `build/NvidiaNimProxy.sln` in Visual Studio 2019+, or use the CMake integration directly in VS.

## Configuration

### API Keys

Provide keys via environment variable (comma-separated) or a local `keys.txt` file (one key per line, `#` comments):

```bash
export NVIDIA_API_KEY="nvapi-xxx,nvapi-yyy,nvapi-zzz"
```

```
# keys.txt
nvapi-aaa
nvapi-bbb
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NVIDIA_API_KEY` | — | Comma-separated API keys |
| `NVIDIA_BASE_URL` / `NIM_BASE_URL` | `https://integrate.api.nvidia.com/v1` | Upstream NIM base URL |
| `KEY_COOLDOWN_SECONDS` | `60` | Base cooldown after a rate-limit failure |
| `KEY_MAX_COOLDOWN_SECONDS` | `1800` | Maximum exponential backoff cap |
| `NIM_MODEL_MAP` | — | JSON object overriding the built-in model map: `{"claude-sonnet-4": "meta/llama-3.1-405b-instruct"}` |

### Key Rotation Behavior

- **Round-robin** cursor advances across the pool; skips keys in cooldown
- **Adaptive backoff**: Each consecutive failure doubles the cooldown (`base × 2^(failures-1)`, capped at `KEY_MAX_COOLDOWN_SECONDS`)
- **Error classification**: Auth failures (401/403) start at 300s base; rate limits (429) use `KEY_COOLDOWN_SECONDS`; server errors (5xx) use ¼ of base
- **Recovery**: A single success resets the failure streak and clears cooldown immediately
- **Degraded mode**: When all keys are in cooldown, the soonest-available key is returned instead of failing

### Client-Side Rate Limiting

- Max 2 concurrent requests (semaphore)
- Minimum 1.71s between sequential requests (~35 RPM safe margin for free tier)
- Global backoff triggerable from handlers

## TUI Control Panel

The TUI starts automatically on launch and takes over the terminal. It provides four tabs:

### Tab 1 — Overview
- Total requests, successes, failures, rate limits, auth failures, server errors
- Success rate, requests/min, active streams
- Latency: average, P95, P99, min/max
- Key health table (state, request counts, cooldown remaining)
- Throughput chart (requests per 5-second bucket)
- Latency chart (average latency per 5-second bucket)
- Sparklines at the bottom

### Tab 2 — Keys
- Per-key detail with success-rate bar chart
- Request/success/failure/consecutive-failure counts
- Cooldown remaining

### Tab 3 — Logs
- Live activity log (color-coded, scrollable with arrow keys)

### Tab 4 — Providers (NEW)
- Configure and manage different AI providers (NVIDIA NIM, OpenAI, Anthropic, Groq, etc.)
- Toggle providers on/off, adjust routing priority, cycle simulated status
- Displays name, type, base URL, enabled state, priority, and health status
- Keyboard controls inside the tab: Up/Down to select, E toggle enable, P cycle priority, S cycle status, R reset

### Controls

| Key | Action |
|-----|--------|
| `1` | Overview tab |
| `2` | Keys tab |
| `3` | Logs tab |
| `4` | Providers tab |
| `Tab` | Cycle tabs |
| `↑` `↓` | Scroll (logs) / Navigate providers |
| `E` | (Providers) Toggle enabled |
| `P` | (Providers) Cycle priority |
| `S` | (Providers) Cycle status |
| `R` | (Providers) Reset selected |
| `q` / `Ctrl-C` | Shut down proxy |

## Architecture

```
Client (Anthropic/OpenAI protocol)
    │
    ▼
┌─────────────────────────────────────┐
│  cpp-httplib Server (:8100)         │
│  ├─ /v1/messages  (Anthropic)       │
│  ├─ /v1/chat/completions (OpenAI)   │
│  ├─ /v1/models    (cached catalog)  │
│  ├─ /v1/keys      (health snapshot) │
│  └─ /v1/(.*)      (wildcard)        │
├─────────────────────────────────────┤
│  KeyManager (round-robin + backoff) │
│  ClientSideBackoff (concurrency)    │
│  ModelCache (1h TTL)                │
│  StatsCollector (rolling windows)   │
│  TUIPanel (ANSI fullscreen)         │
├─────────────────────────────────────┤
│  libcurl → NVIDIA NIM API           │
└─────────────────────────────────────┘
```

### Source Layout

| File | Purpose |
|------|---------|
| `main.cpp` | HTTP server, route handlers, proxy logic |
| `proxy_config.h` | Network types: SafeQueue, ProxyContext, curl callbacks, streaming state |
| `key_manager.h` | KeyManager, ClientSideBackoff, ModelCache |
| `anthropic_handler.h/.cpp` | Anthropic ↔ OpenAI request/response translation, SSE stream processing |
| `utils.h/.cpp` | Model mapping, token estimation, key loading |
| `logger.h` | Thread-safe logger with file output and TUI callback |
| `stats_collector.h` | Rolling-window statistics, latency percentiles, time series |
| `tui_panel.h` | Fullscreen ANSI terminal UI with charts and keybindings |

### Dependencies (Bundled)

| Dependency | Version | Location |
|------------|---------|----------|
| libcurl | 8.21.0 | `thirdparty/curl` (git submodule) |
| nlohmann/json | 3.12.0 | `include/nlohmann/json.hpp` (single header) |
| cpp-httplib | 0.51.0 | `include/httplib.h` (single header) |

All dependencies are bundled in the repository — no external package manager required.

## Usage with Clients

### OpenAI-compatible

```bash
curl http://127.0.0.1:8100/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"meta/llama-3.1-405b-instruct","messages":[{"role":"user","content":"Hello"}]}'
```

### Anthropic-compatible

```bash
curl http://127.0.0.1:8100/v1/messages \
  -H "Content-Type: application/json" \
  -H "anthropic-version: 2023-06-01" \
  -d '{"model":"claude-sonnet-4","max_tokens":1024,"messages":[{"role":"user","content":"Hello"}]}'
```

### With OpenAI SDK

```python
from openai import OpenAI
client = OpenAI(base_url="http://127.0.0.1:8100/v1", api_key="unused")
resp = client.chat.completions.create(
    model="meta/llama-3.1-405b-instruct",
    messages=[{"role": "user", "content": "Hello"}]
)
```

### With Anthropic SDK

```python
from anthropic import Anthropic
client = Anthropic(base_url="http://127.0.0.1:8100", api_key="unused")
resp = client.messages.create(
    model="claude-sonnet-4",
    max_tokens=1024,
    messages=[{"role": "user", "content": "Hello"}]
)
```

## License

See LICENSE file (if present) or use as-is.

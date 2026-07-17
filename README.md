# Windows

### Install deps

```
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# Install cross-platform HTTP parser and curl for standard MSVC targets
.\vcpkg.exe install curl nlohmann-json cpp-httplib
```

### Build

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

# Linux

### Install deps (Ubuntu/Debian)

```
sudo apt install build-essential cmake libcurl4-openssl-dev nlohmann-json3-dev
```

#### Termux

```
pkg install clang cmake libcurl nlohmann-json
```

Download httplib.h locally (if you don't have it installed globally):

```
curl -LO https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
```

### Build

```
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

# Usage

1. Set the NVIDIA_API_KEY env, separate your API keys with commas
2. Run the binary and add the server to OpenAI Compatible agents

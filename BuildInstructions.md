Commands to run from the project root to produce a signed `.pkg`:

```bash
# 1. Set your signing identity (replace with your own values from your Apple Developer account)
export CODESIGN_IDENT="Developer ID Application: Your Name (YOURTEAMID)"
export CODESIGN_TEAM="YOURTEAMID"

# 2. Configure (only needed the first time, or after changing CMakeLists.txt)
cmake --preset macos

# 3. Build
cmake --build --preset macos

# 4. Install + generate .pkg
cmake --install build_macos --config RelWithDebInfo --prefix "$(pwd)/release/RelWithDebInfo"
```

After step 4 finishes, your installer will be at:
```
release/RelWithDebInfo/kxtsune-obs.pkg
```

### On subsequent builds (no CMake changes)
You only need steps 1, 3, and 4 — skip step 2 unless you've changed `CMakeLists.txt` or `buildspec.json`.

### Quick one-liner (copy-paste for future use)
```bash
export CODESIGN_IDENT="Developer ID Application: Your Name (YOURTEAMID)" && \
export CODESIGN_TEAM="YOURTEAMID" && \
cmake --preset macos && \
cmake --build --preset macos && \
cmake --install build_macos --config RelWithDebInfo --prefix "$(pwd)/release/RelWithDebInfo"
```

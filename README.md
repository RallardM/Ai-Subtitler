# Ai-Subtitler (Streamer.bot + whisper.cpp)

Windows app that:

1. Captures microphone audio (SDL2)
2. Transcribes speech with Whisper (via the `submodules/whisper.cpp` submodule)
3. Sends each finalized text block to Streamer.bot via WebSocket `DoAction`

## Dependencies

### Build-time

- Windows 10/11
- Git
- CMake 3.16+
- Visual Studio 2022 (or Build Tools) with **Desktop development with C++**
  - MSVC toolchain
  - Windows 10/11 SDK

SDL2 is required for microphone capture. This repo will try to:

- Use an existing SDL2 install if CMake can find it, otherwise
- Fetch and build SDL2 automatically (default: `-DAI_SUBTITLER_FETCH_SDL2=ON`)

### Run-time

- A Whisper model file on disk (NOT included in git; too large for GitHub)
- Streamer.bot with WebSocket server enabled (default is usually `ws://127.0.0.1:8080/`)

## Clone / setup

This repo uses a git submodule (`submodules/whisper.cpp`).

```powershell
git clone --recurse-submodules https://github.com/RallardM/Ai-Subtitler.git
cd Ai-Subtitler
```

If you already cloned without submodules:

```powershell
git submodule update --init --recursive
```

## Build (Windows)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

- `build/Release/ai-subtitler-streamerbot.exe`

The build copies required DLLs next to the exe automatically.

## Download a model

Put model files under `models/` (this folder is ignored by git).

Example using whisper.cpp’s downloader:

```powershell
New-Item -ItemType Directory -Force models | Out-Null
submodules\whisper.cpp\models\download-ggml-model.cmd medium .\models
```

This typically produces `models/ggml-medium.bin` (exact name depends on the model).

## Run

### Quick start (one-click)

This is preconfigured for:

- Model: `models\ggml-medium.bin`
- Device index: `1`
- Streamer.bot WS: `ws://127.0.0.1:8080/`
- Action name: `AI Subtitler` (arg key: `AiText`)

```powershell
./start-ai-subtitler.cmd
```

If you want to explicitly select the mic (recommended because device indexes can change), pass either:

- An index: `0`, `1`, ...
- A name substring (more stable): e.g. `"Samson"`

Examples:

```powershell
# mic by index
./start-ai-subtitler.cmd 0

# mic by name substring
./start-ai-subtitler.cmd "Samson"
```

PowerShell tip: if you ever run into execution quirks, this form also works:

```powershell
& .\start-ai-subtitler.cmd 0
```

VS Code tip: don’t paste commands that look like `[file](http://_vscodecontentref_/...)` into PowerShell — that’s a Markdown link and PowerShell will try to execute the `http://...` part.

### Choose your microphone

List capture devices:

```powershell
./list-devices.cmd
```

If you run the exe without specifying `--device-index` or `--device-name`, it will list devices and prompt you to pick one (interactive terminals only).

Then run (example):

```powershell
./run.cmd --model .\models\ggml-medium.bin --device-name "Samson" --language en --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText
```

If you enabled WebSocket authentication in Streamer.bot:

```powershell
./run.cmd --model .\models\ggml-medium.bin --device-index 1 --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --ws-password "your_password"
```

## Notes

- Large model binaries are intentionally ignored (GitHub rejects files > 100 MB).
- If SDL2 is not found and you disabled auto-fetch, set `SDL2_DIR`/`SDL2_ROOT` to an SDL2 dev package (or install SDL2 via vcpkg) and re-configure CMake.

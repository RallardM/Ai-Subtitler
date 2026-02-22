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

Default (recommended) model for speed:

```powershell
New-Item -ItemType Directory -Force models | Out-Null
submodules\whisper.cpp\models\download-ggml-model.cmd tiny .\models
```

Release ZIP users (no git submodule): use the explicit download scripts (easier to tell apart):

```powershell
.\download-model-tiny-multilanguage.cmd
.\download-model-tiny-en.cmd
```

This typically produces `models/ggml-tiny.bin` (exact name depends on the model).

Notes:

- For English-only (usually a bit faster), download `tiny.en` instead:

  ```powershell
  submodules\whisper.cpp\models\download-ggml-model.cmd tiny.en .\models
  ```

## Run

### Quick start (one-click)

This is preconfigured for:

- Model: uses a model picker. It lists Whisper models found under `models\` and asks you to pick one by number for the current run.
- Streamer.bot WS: `ws://127.0.0.1:8080/`
- Action name: `AI Subtitler` (arg key: `AiText`)

The one-click launcher uses `--fast` by default to reduce latency (faster, less accurate).

```powershell
.\start-ai-subtitler.cmd
```

If you want to explicitly select the mic (recommended because device indexes can change), pass either:

- An index: `0`, `1`, ...
- A name substring (more stable): e.g. `"Samson"`

Examples:

```powershell
# mic by index
.\start-ai-subtitler.cmd 0

# mic by name substring
.\start-ai-subtitler.cmd "Samson"
```

Tip: you can also pass extra flags after the mic selection, for example:

```powershell
.\start-ai-subtitler.cmd 0 --debug-thankyou
```

In the Release ZIP, you can double-click `Start (Fast, Mic 0).cmd` (portable launcher). It already keeps the console open.

If you launch via a Windows shortcut and the console closes too quickly, add `--keep-open` (launcher-only) to keep the window open after the app exits:

```powershell
.\start-ai-subtitler.cmd 0 --keep-open
```

If you want to skip the interactive model picker from a shortcut, pass either:

- `--model <path>` (explicit file)
- `--model-index N` (launcher-only; chooses the Nth model listed from `models\`)

Examples:

```powershell
.\start-ai-subtitler.cmd 0 --model-index 0
.\start-ai-subtitler.cmd 0 --model .\models\ggml-tiny.en.bin
```

### Voice gate (speech vs noise)

By default, Ai-Subtitler uses a small Silero VAD model to detect real voice. This voice gate is used to decide what audio gets flushed and sent to Whisper.

It does **not** require a special argument to enable it.
It is active automatically when `models/ggml-silero-v6.2.0.bin` exists and the app prints `Voice gate: ON (Silero) ...` on startup.

When active, the app will only send audio to Whisper after:

- You spoke for at least `--min-voice-ms` (default: 600ms)
- Then you stopped speaking for `--voice-stop-ms` (default: 3000ms)

This helps avoid transcribing keyboard clicks, music, or silence.

#### Download the VAD model

The Silero VAD model is **separate** from the Whisper ASR model.

```powershell
.\download-vad.cmd
```

This downloads `models/ggml-silero-v6.2.0.bin`.

#### Confirm you are testing the voice gate (important)

On startup, the app prints a banner showing the actual mode:

- `Voice gate: ON (Silero) ...`  (this means the voice detection system is active)
- `Voice gate: OFF (--no-voice-gate)`
- `Voice gate: OFF (fallback to simple VAD; missing/failed Silero model) ...`

If you see a fallback message, run `.\download-vad.cmd`.

To disable the voice gate (and use the simple silence-tail VAD), run with `--no-voice-gate`.

#### Debug voice detection only (no Whisper, no Streamer.bot)

```powershell
.\debug-voice-gate.cmd 0
```

It prints only:

- `DETECT VOICE`
- `DOES NOT DETECT VOICE`

#### Trace voice gate events (live mic)

If you want to *prove* the gating behavior on your microphone, add `--trace-voice-gate`.

Release ZIP example (one-click launcher + tracing):

```powershell
.\start-ai-subtitler.cmd 0 --trace-voice-gate
```

```powershell
.\run.ps1 --model .\models\ggml-tiny.bin --mic 0 --fast --trace-voice-gate
```

This prints events like:

- `[VG] VOICE_START ...`
- `[VG] VOICE_END ...`
- `[VG] FLUSH ...` (this is when Whisper will run)

#### Deterministic offline test (no mic)

There is also an offline test mode that runs the voice gate on an audio file and prints the same events.

This repo already includes a sample file from the whisper.cpp submodule:

```powershell
.\run.ps1 --test-voice-gate submodules\whisper.cpp\samples\jfk.wav --vad-model .\models\ggml-silero-v6.2.0.bin
```

PowerShell tip: if you ever run into execution quirks, this form also works:

```powershell
& .\start-ai-subtitler.cmd 0
```

VS Code tip: if copy/paste gets mangled, type the command manually or use VS Code’s `Terminal -> Run Task...`.

### Run the EXE directly (shortcut-friendly)

If you prefer a Windows shortcut pointing to the executable directly, the app supports:

- `mic_index` as a positional argument (example: `ai-subtitler-streamerbot.exe 0`)
- `--model` as optional when `./models` contains one of these filenames:
  - `models/ggml-tiny.bin`
  - `models/ggml-tiny.en.bin`
  - `models/ggml-medium.bin`

Examples:

```powershell
# prompts for device (interactive terminals only)
.\build\Release\ai-subtitler-streamerbot.exe --fast

# skip prompt by passing the device index positionally
.\build\Release\ai-subtitler-streamerbot.exe 0 --fast

# explicit model always works (regardless of filename)
.\build\Release\ai-subtitler-streamerbot.exe 0 --model .\models\ggml-tiny.bin --fast
```

### Speed vs accuracy

- Faster (default via `start-ai-subtitler.cmd`): uses `--fast`
- More accurate (slower): run via `run.cmd`/`run.ps1` and omit `--fast` (and/or use longer blocks)

Example (more accurate, slower):

```powershell
.\run.ps1 --model (Resolve-Path .\models\ggml-medium.bin) --mic 0 --length-ms 30000
```

Ultra-low latency tuning (advanced):

```powershell
.\run.cmd --model .\models\ggml-tiny.bin --mic 0 --fast --length-ms 1500 --vad-check-ms 80 --vad-window-ms 600 --vad-last-ms 200 --max-tokens 16 --dedup-similarity 0.75
```

Note: these values are extremely aggressive and can chop sentences on normal speech. If that happens, increase `--vad-last-ms`/`--vad-window-ms` and `--length-ms`.

### Choose your microphone

List capture devices:

```powershell
.\list-devices.cmd
```

If you run the exe without specifying `--device-index` or `--device-name`, it will list devices and prompt you to pick one (interactive terminals only).

Then run (example):

```powershell
.\run.cmd --model .\models\ggml-tiny.bin --mic "Samson" --language en --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText
```

If you enabled WebSocket authentication in Streamer.bot:

```powershell
.\run.cmd --model .\models\ggml-tiny.bin --mic 0 --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --ws-password "your_password"
```

## Notes

- Large model binaries are intentionally ignored (GitHub rejects files > 100 MB).
- If SDL2 is not found and you disabled auto-fetch, set `SDL2_DIR`/`SDL2_ROOT` to an SDL2 dev package (or install SDL2 via vcpkg) and re-configure CMake.

# Ai-Subtitler (Streamer.bot + whisper.cpp)

This repo contains a small executable that:

1. Captures microphone audio (SDL2)
2. Runs Whisper (via the `submodules/whisper.cpp` submodule)
3. Sends each finalized “utterance block” to Streamer.bot via WebSocket `DoAction`

## Build (Windows)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output (Release):

- `build/Release/ai-subtitler-streamerbot.exe`

The build copies required DLLs next to the exe automatically.

## Download a model

You can use whisper.cpp’s model downloader:

```powershell
# download to a local folder (example: .\models)
New-Item -ItemType Directory -Force models | Out-Null
submodules\whisper.cpp\models\download-ggml-model.cmd medium .\models
```

That produces: `models/ggml-medium.bin` (or similar, depending on the model name).

## Run while streaming

Fastest way (preconfigured for: device index 1, `models/ggml-medium.bin`, Streamer.bot `ws://127.0.0.1:8080/`, action `AI Subtitler`, arg key `AiText`):

```powershell
./start-ai-subtitler.cmd
```

1) List capture devices (to match OBS’s Mic/Aux device):

```powershell
./list-devices.cmd
```

2) Run transcription + send to Streamer.bot Action:

```powershell
./run.cmd \
  --model .\models\ggml-medium.bin \
  --device-name "Samson" \
  --language en \
  --ws-url ws://127.0.0.1:8080/ \
  --action-name "AI Subtitler" \
  --arg-key AiText
```

If you enabled WebSocket authentication in Streamer.bot, add:

```powershell
  --ws-password "your_password"
```

The executable uses Streamer.bot’s `DoAction` request and passes the transcript as an argument named `AiText`.

### Language behavior

- Default is English (`en`).
- If the audio block is more likely French, it will automatically switch to French (`fr`).
- To force a specific language (or restore full auto-detect), pass `--language <code>` (e.g. `--language fr` or `--language auto`).

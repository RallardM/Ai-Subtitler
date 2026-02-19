#include "streamerbot_ws_client.h"

#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"

#include "ggml-backend.h"
#include "whisper.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

struct app_params {
    // whisper
    std::string model;
    std::string language = "auto";
    int32_t threads = std::max(1, (int32_t) std::thread::hardware_concurrency() - 1);
    bool translate = false;
    bool use_gpu = true;
    bool flash_attn = true;

    // VAD streaming
    int32_t length_ms = 30000;  // audio window captured on silence
    float vad_thold = 0.60f;
    float freq_thold = 100.0f;

    // audio device
    bool list_devices = false;
    int32_t device_index = -1;
    std::string device_name_substring;

    // streamer.bot
    streamerbot_ws_config bot;

    // misc
    float dedup_similarity = 0.90f;
};

static std::string trim_and_collapse_ws(const std::string & s) {
    std::string out;
    out.reserve(s.size());

    bool in_ws = false;
    for (unsigned char ch : s) {
        const bool is_ws = std::isspace(ch) != 0;
        if (is_ws) {
            in_ws = true;
            continue;
        }
        if (in_ws && !out.empty()) {
            out.push_back(' ');
        }
        in_ws = false;
        out.push_back((char) ch);
    }

    // trim leading/trailing spaces
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static bool icontains(const std::string & haystack, const std::string & needle) {
    if (needle.empty()) return true;
    auto tolower_u = [](unsigned char c) { return (unsigned char) std::tolower(c); };

    std::string h = haystack;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [&](unsigned char c) { return (char) tolower_u(c); });
    std::transform(n.begin(), n.end(), n.begin(), [&](unsigned char c) { return (char) tolower_u(c); });
    return h.find(n) != std::string::npos;
}

static void print_usage(const char * exe) {
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "Usage: %s --model <path> [options]\n\n", exe);
    std::fprintf(stderr, "Whisper:\n");
    std::fprintf(stderr, "  --model <path>            Path to ggml model (required)\n");
    std::fprintf(stderr, "  --language <auto|en|...>  Spoken language (default: auto)\n");
    std::fprintf(stderr, "  --threads N               Threads (default: cores-1)\n");
    std::fprintf(stderr, "  --translate               Translate to English\n");
    std::fprintf(stderr, "  --no-gpu                  Disable GPU inference\n");
    std::fprintf(stderr, "  --no-flash-attn           Disable flash-attn\n\n");

    std::fprintf(stderr, "Audio/VAD:\n");
    std::fprintf(stderr, "  --list-devices            List capture devices and exit\n");
    std::fprintf(stderr, "  --device-index N          Capture device index (SDL2)\n");
    std::fprintf(stderr, "  --device-name <substring> Capture device name substring (preferred)\n");
    std::fprintf(stderr, "  --length-ms N             Window length for VAD blocks (default: 30000)\n");
    std::fprintf(stderr, "  --vad-thold X             VAD threshold (default: 0.60)\n");
    std::fprintf(stderr, "  --freq-thold X            High-pass cutoff (default: 100.0)\n\n");

    std::fprintf(stderr, "Streamer.bot:\n");
    std::fprintf(stderr, "  --ws-url ws://127.0.0.1:8080/   WebSocket URL\n");
    std::fprintf(stderr, "  --ws-password <pwd>       Optional WebSocket password\n");
    std::fprintf(stderr, "  --action-name \"AI Subtitler\"   Action to execute\n");
    std::fprintf(stderr, "  --arg-key AiText           Argument key (default: AiText)\n\n");
}

static bool parse_args(int argc, char ** argv, app_params & p) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto require_value = [&](const char * name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--model") {
            p.model = require_value("--model");
        } else if (arg == "--language") {
            p.language = require_value("--language");
        } else if (arg == "--threads") {
            p.threads = std::stoi(require_value("--threads"));
        } else if (arg == "--translate") {
            p.translate = true;
        } else if (arg == "--no-gpu") {
            p.use_gpu = false;
        } else if (arg == "--no-flash-attn") {
            p.flash_attn = false;
        } else if (arg == "--list-devices") {
            p.list_devices = true;
        } else if (arg == "--device-index") {
            p.device_index = std::stoi(require_value("--device-index"));
        } else if (arg == "--device-name") {
            p.device_name_substring = require_value("--device-name");
        } else if (arg == "--length-ms") {
            p.length_ms = std::stoi(require_value("--length-ms"));
        } else if (arg == "--vad-thold") {
            p.vad_thold = std::stof(require_value("--vad-thold"));
        } else if (arg == "--freq-thold") {
            p.freq_thold = std::stof(require_value("--freq-thold"));
        } else if (arg == "--ws-url") {
            p.bot.url = require_value("--ws-url");
        } else if (arg == "--ws-password") {
            p.bot.password = std::string(require_value("--ws-password"));
        } else if (arg == "--action-name") {
            p.bot.action_name = require_value("--action-name");
        } else if (arg == "--arg-key") {
            p.bot.arg_key = require_value("--arg-key");
        } else {
            std::fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

static bool sdl_list_devices_only() {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    const int n = SDL_GetNumAudioDevices(SDL_TRUE);
    std::printf("Found %d capture devices:\n", n);
    for (int i = 0; i < n; ++i) {
        const char * name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        std::printf("  [%d] %s\n", i, name ? name : "(null)");
    }

    SDL_Quit();
    return true;
}

static int sdl_find_device_index_by_substring(const std::string & needle) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    const int n = SDL_GetNumAudioDevices(SDL_TRUE);
    for (int i = 0; i < n; ++i) {
        const char * name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        if (name && icontains(name, needle)) {
            SDL_Quit();
            return i;
        }
    }

    SDL_Quit();
    return -1;
}

int main(int argc, char ** argv) {
    ggml_backend_load_all();

    app_params params;
    if (!parse_args(argc, argv, params)) {
        print_usage(argv[0]);
        return 1;
    }

    if (params.list_devices) {
        return sdl_list_devices_only() ? 0 : 2;
    }

    if (params.model.empty()) {
        std::fprintf(stderr, "error: --model is required\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!params.device_name_substring.empty()) {
        const int idx = sdl_find_device_index_by_substring(params.device_name_substring);
        if (idx < 0) {
            std::fprintf(stderr, "error: no capture device matched --device-name '%s'\n", params.device_name_substring.c_str());
            return 2;
        }
        params.device_index = idx;
        std::fprintf(stderr, "Using capture device index %d (matched by name substring)\n", params.device_index);
    }

    // init audio capture (reuse whisper.cpp example helper)
    audio_async audio(params.length_ms);
    if (!audio.init(params.device_index, WHISPER_SAMPLE_RATE)) {
        std::fprintf(stderr, "error: audio.init() failed\n");
        return 3;
    }
    audio.resume();

    // init whisper
    if (params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1) {
        std::fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        return 4;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;

    whisper_context * ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        std::fprintf(stderr, "error: failed to initialize whisper context\n");
        return 5;
    }

    if (!whisper_is_multilingual(ctx)) {
        if (params.language != "en" || params.translate) {
            std::fprintf(stderr, "warning: model is not multilingual; forcing language=en and translate=false\n");
            params.language = "en";
            params.translate = false;
        }
    }

    // connect to Streamer.bot (keep retrying)
    streamerbot_ws_client bot;
    {
        std::string err;
        for (int attempt = 0; attempt < 1000000; ++attempt) {
            if (bot.connect_and_handshake(params.bot, err)) {
                std::fprintf(stderr, "Connected to Streamer.bot WebSocket: %s\n", params.bot.url.c_str());
                break;
            }
            std::fprintf(stderr, "Streamer.bot connect failed (%s). Retrying in 2s...\n", err.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    std::vector<float> pcm_2s;
    std::vector<float> pcm_block;
    std::string last_sent;

    std::puts("[Start speaking]");
    std::fflush(stdout);

    auto t_last = std::chrono::high_resolution_clock::now();
    bool running = true;
    int iter = 0;

    while (running) {
        running = sdl_poll_events();
        if (!running) break;

        const auto t_now = std::chrono::high_resolution_clock::now();
        const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last).count();
        if (t_diff < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        audio.get(2000, pcm_2s);
        if (pcm_2s.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // In whisper.cpp, vad_simple() returns true when the last part of the window is relatively silent.
        if (!::vad_simple(pcm_2s, WHISPER_SAMPLE_RATE, 1000, params.vad_thold, params.freq_thold, false)) {
            t_last = t_now;
            continue;
        }

        audio.get(params.length_ms, pcm_block);
        if (pcm_block.size() < (size_t) (WHISPER_SAMPLE_RATE * 0.5)) {
            t_last = t_now;
            continue;
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.print_special = false;
        wparams.print_timestamps = false;
        wparams.translate = params.translate;
        wparams.single_segment = false;
        wparams.max_tokens = 0;
        wparams.language = params.language.c_str();
        wparams.n_threads = params.threads;
        wparams.audio_ctx = 0;

        if (whisper_full(ctx, wparams, pcm_block.data(), pcm_block.size()) != 0) {
            std::fprintf(stderr, "whisper_full failed\n");
            t_last = t_now;
            continue;
        }

        std::string text;
        const int n_segments = whisper_full_n_segments(ctx);
        for (int i = 0; i < n_segments; ++i) {
            const char * seg = whisper_full_get_segment_text(ctx, i);
            if (seg) text += seg;
        }

        text = trim_and_collapse_ws(text);
        if (text.empty()) {
            t_last = t_now;
            continue;
        }

        // De-dupe: skip very similar repeats (common with sliding windows).
        if (!last_sent.empty()) {
            const float sim = ::similarity(last_sent, text);
            if (sim >= params.dedup_similarity) {
                t_last = t_now;
                continue;
            }
        }

        // Send to Streamer.bot (retry once on disconnect)
        {
            std::string err;
            if (!bot.is_connected()) {
                bot.connect_and_handshake(params.bot, err);
            }
            if (!bot.do_action_text(params.bot, text, err)) {
                std::fprintf(stderr, "DoAction failed (%s). Reconnecting...\n", err.c_str());
                bot.close();
                if (bot.connect_and_handshake(params.bot, err)) {
                    (void) bot.do_action_text(params.bot, text, err);
                }
            }
        }

        last_sent = text;
        std::printf("[%d] %s\n", iter++, text.c_str());
        std::fflush(stdout);

        t_last = t_now;
    }

    audio.pause();
    whisper_print_timings(ctx);
    whisper_free(ctx);
    return 0;
}

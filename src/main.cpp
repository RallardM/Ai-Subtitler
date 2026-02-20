#include "streamerbot_ws_client.h"

#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"

#include "ggml-backend.h"
#include "whisper.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    include <io.h>
#else
#    include <unistd.h>
#endif

struct app_params {
    // whisper
    std::string model;
    // Default to English, with an automatic fallback to French if detection strongly suggests it.
    std::string language = "en";
    int32_t threads = std::max(1, (int32_t) std::thread::hardware_concurrency() - 1);
    bool translate = false;
    bool use_gpu = true;
    bool flash_attn = true;

    // speed/accuracy preset
    bool fast = false;
    int32_t max_tokens = 0;

    // VAD streaming
    int32_t length_ms = 30000;  // audio window captured on silence
    int32_t vad_check_ms = 2000;   // how often we evaluate VAD and decide to flush
    int32_t vad_window_ms = 2000;  // audio window used for VAD evaluation
    int32_t vad_last_ms = 1000;    // trailing part of vad_window_ms that must be relatively silent
    float vad_thold = 0.60f;
    float freq_thold = 100.0f;

    // audio device
    bool list_devices = false;
    int32_t device_index = -1;
    std::string device_name_substring;

    // streamer.bot
    streamerbot_ws_config bot;

    std::string startup_text;

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

static std::vector<std::string> split_words_lower_ascii(const std::string & s) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(16);

    auto flush = [&]() {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };

    for (unsigned char ch : s) {
        if (std::isalnum(ch)) {
            cur.push_back((char) std::tolower(ch));
        } else {
            flush();
        }
    }
    flush();
    return out;
}

static bool ends_with_words(const std::vector<std::string> & words, const std::vector<std::string> & suffix) {
    if (suffix.empty() || suffix.size() > words.size()) return false;
    const size_t start = words.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (words[start + i] != suffix[i]) {
            return false;
        }
    }
    return true;
}

// Detect the common "same sentence, but missing the first word" streaming artifact.
// Returns true if `cur` is a word-suffix of `prev` (and not trivially short).
static bool is_suffix_repeat_by_words(const std::string & prev, const std::string & cur) {
    const auto w_prev = split_words_lower_ascii(prev);
    const auto w_cur = split_words_lower_ascii(cur);
    if (w_cur.size() < 3) return false;
    if (w_prev.size() <= w_cur.size()) return false;
    return ends_with_words(w_prev, w_cur);
}

static float audio_activity_fraction(const std::vector<float> & pcm, float abs_thold) {
    if (pcm.empty()) return 0.0f;
    size_t n_active = 0;
    for (float v : pcm) {
        if (std::fabs(v) > abs_thold) {
            ++n_active;
        }
    }
    return (float) n_active / (float) pcm.size();
}

static bool is_exact_thank_you(const std::string & s) {
    const auto w = split_words_lower_ascii(s);
    if (w.size() == 2 && w[0] == "thank" && w[1] == "you") return true;
    if (w.size() == 1 && w[0] == "thankyou") return true;
    return false;
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
    std::fprintf(stderr, "  --language <auto|en|...>  Spoken language (default: en; auto-fallback to fr when likely)\n");
    std::fprintf(stderr, "  --threads N               Threads (default: cores-1)\n");
    std::fprintf(stderr, "  --translate               Translate to English\n");
    std::fprintf(stderr, "  --no-gpu                  Disable GPU inference\n");
    std::fprintf(stderr, "  --no-flash-attn           Disable flash-attn\n\n");

    std::fprintf(stderr, "Presets:\n");
    std::fprintf(stderr, "  --fast                    Faster, less accurate (shorter blocks, no extra language-detect pass, more aggressive decoding)\n\n");

    std::fprintf(stderr, "Audio/VAD:\n");
    std::fprintf(stderr, "  --list-devices            List capture devices and exit\n");
    std::fprintf(stderr, "  --mic <N|substring>       Microphone selection shortcut: index (e.g. --mic 0) or name substring (e.g. --mic Samson)\n");
    std::fprintf(stderr, "  --device-index N          Capture device index (SDL2)\n");
    std::fprintf(stderr, "  --device-name <substring> Capture device name substring (preferred)\n");
    std::fprintf(stderr, "                           If neither is provided, the app will list devices and prompt (interactive shells only)\n");
    std::fprintf(stderr, "  --length-ms N             Window length for VAD blocks (default: 30000)\n");
    std::fprintf(stderr, "  --vad-check-ms N          How often to evaluate VAD (default: 2000; fast preset: 100)\n");
    std::fprintf(stderr, "  --vad-window-ms N         Window size used for VAD evaluation (default: 2000; fast preset: 800)\n");
    std::fprintf(stderr, "  --vad-last-ms N           Trailing tail that must be quiet to flush (default: 1000; fast preset: 350)\n");
    std::fprintf(stderr, "  --vad-thold X             VAD threshold (default: 0.60)\n");
    std::fprintf(stderr, "  --freq-thold X            High-pass cutoff (default: 100.0)\n\n");

    std::fprintf(stderr, "Decoding:\n");
    std::fprintf(stderr, "  --max-tokens N            Max tokens per block (0 = no limit; fast preset: 32)\n\n");

    std::fprintf(stderr, "Streamer.bot:\n");
    std::fprintf(stderr, "  --ws-url ws://127.0.0.1:8080/   WebSocket URL\n");
    std::fprintf(stderr, "  --ws-password <pwd>       Optional WebSocket password\n");
    std::fprintf(stderr, "  --action-name \"AI Subtitler\"   Action to execute\n");
    std::fprintf(stderr, "  --arg-key AiText           Argument key (default: AiText)\n\n");

    std::fprintf(stderr, "Diagnostics:\n");
    std::fprintf(stderr, "  --startup-text <text>      Send a DoAction immediately after start (useful to verify Streamer.bot connectivity)\n\n");

    std::fprintf(stderr, "Output filtering:\n");
    std::fprintf(stderr, "  --dedup-similarity X       Skip very similar repeats (default: 0.90; fast preset: 0.80)\n\n");
}

static std::string pick_language_en_fallback_fr(whisper_context * ctx, const std::vector<float> & pcm, int n_threads) {
    // Only try to disambiguate between English and French.
    // Returns "en" unless French is clearly more likely.
    if (!ctx || pcm.empty()) {
        return "en";
    }

    const int rc_mel = whisper_pcm_to_mel(ctx, pcm.data(), (int) pcm.size(), n_threads);
    if (rc_mel != 0) {
        return "en";
    }

    std::vector<float> lang_probs(whisper_lang_max_id() + 1, 0.0f);
    const int detected_id = whisper_lang_auto_detect(ctx, 0, n_threads, lang_probs.data());
    (void) detected_id;

    const int en_id = whisper_lang_id("en");
    const int fr_id = whisper_lang_id("fr");
    const float p_en = (en_id >= 0 && en_id < (int) lang_probs.size()) ? lang_probs[en_id] : 0.0f;
    const float p_fr = (fr_id >= 0 && fr_id < (int) lang_probs.size()) ? lang_probs[fr_id] : 0.0f;

    // Conservative switch to French: it must beat English and also be non-trivial.
    if (p_fr > p_en && p_fr >= 0.50f) {
        return "fr";
    }

    return "en";
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
        } else if (arg == "--fast") {
            p.fast = true;
            // Preset tuned for lower latency at the cost of accuracy.
            // Users can still override these later in the CLI.
            // A shorter decode window reduces end-to-end delay.
            p.length_ms = 3500;
            // Reduce the "wait to flush" latency by checking VAD frequently and requiring a shorter silence tail.
            p.vad_check_ms = 100;
            p.vad_window_ms = 800;
            p.vad_last_ms = 350;
            // Reduce decoder work.
            p.max_tokens = 32;
            // More aggressive de-dupe to avoid repeated overlap spam.
            p.dedup_similarity = 0.80f;
            p.threads = std::max(1, (int32_t) std::thread::hardware_concurrency());
        } else if (arg == "--list-devices") {
            p.list_devices = true;
        } else if (arg == "--mic") {
            const std::string v = require_value("--mic");
            // If it's an integer, treat as index. Otherwise treat as substring.
            // Accept leading +/-, but only allow non-negative indices.
            bool all_digits = !v.empty();
            size_t start = 0;
            if (v.size() >= 1 && (v[0] == '+' || v[0] == '-')) {
                start = 1;
            }
            for (size_t k = start; k < v.size(); ++k) {
                if (!std::isdigit((unsigned char) v[k])) {
                    all_digits = false;
                    break;
                }
            }

            if (all_digits) {
                const int idx = std::stoi(v);
                if (idx < 0) {
                    std::fprintf(stderr, "error: --mic index must be >= 0\n");
                    return false;
                }
                p.device_index = idx;
            } else {
                p.device_name_substring = v;
            }
        } else if (arg == "--device-index") {
            p.device_index = std::stoi(require_value("--device-index"));
        } else if (arg == "--device-name") {
            p.device_name_substring = require_value("--device-name");
        } else if (arg == "--length-ms") {
            p.length_ms = std::stoi(require_value("--length-ms"));
        } else if (arg == "--vad-check-ms") {
            p.vad_check_ms = std::stoi(require_value("--vad-check-ms"));
        } else if (arg == "--vad-window-ms") {
            p.vad_window_ms = std::stoi(require_value("--vad-window-ms"));
        } else if (arg == "--vad-last-ms") {
            p.vad_last_ms = std::stoi(require_value("--vad-last-ms"));
        } else if (arg == "--vad-thold") {
            p.vad_thold = std::stof(require_value("--vad-thold"));
        } else if (arg == "--freq-thold") {
            p.freq_thold = std::stof(require_value("--freq-thold"));
        } else if (arg == "--max-tokens") {
            p.max_tokens = std::stoi(require_value("--max-tokens"));
        } else if (arg == "--ws-url") {
            p.bot.url = require_value("--ws-url");
        } else if (arg == "--ws-password") {
            p.bot.password = std::string(require_value("--ws-password"));
        } else if (arg == "--action-name") {
            p.bot.action_name = require_value("--action-name");
        } else if (arg == "--arg-key") {
            p.bot.arg_key = require_value("--arg-key");
        } else if (arg == "--startup-text") {
            p.startup_text = require_value("--startup-text");
        } else if (arg == "--dedup-similarity") {
            p.dedup_similarity = std::stof(require_value("--dedup-similarity"));
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

static bool stdin_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

static int sdl_prompt_for_device_index(int default_index) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    const int n = SDL_GetNumAudioDevices(SDL_TRUE);
    std::printf("Found %d capture devices:\n", n);
    for (int i = 0; i < n; ++i) {
        const char * name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        std::printf("  [%d] %s\n", i, name ? name : "(null)");
    }

    if (n <= 0) {
        SDL_Quit();
        return -1;
    }

    if (!stdin_is_tty()) {
        std::fprintf(stderr, "No capture device specified and stdin is not interactive; using SDL default device.\n");
        std::fprintf(stderr, "Tip: run with --list-devices then pass --device-index N (or --device-name).\n");
        SDL_Quit();
        return -1;
    }

    if (default_index < 0 || default_index >= n) {
        default_index = 0;
    }

    while (true) {
        std::printf("Select capture device index [0..%d] (default: %d): ", n - 1, default_index);
        std::fflush(stdout);

        std::string line;
        if (!std::getline(std::cin, line)) {
            SDL_Quit();
            return -1;
        }

        // trim
        line = trim_and_collapse_ws(line);
        if (line.empty()) {
            SDL_Quit();
            return default_index;
        }

        try {
            const int idx = std::stoi(line);
            if (idx >= 0 && idx < n) {
                SDL_Quit();
                return idx;
            }
        } catch (...) {
        }

        std::fprintf(stderr, "Invalid device index.\n");
    }
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

    // Sanity/clamping to avoid invalid VAD windows.
    params.vad_check_ms = std::max<int32_t>(50, params.vad_check_ms);
    params.vad_window_ms = std::max<int32_t>(200, params.vad_window_ms);
    params.vad_last_ms = std::max<int32_t>(50, params.vad_last_ms);
    if (params.vad_last_ms > params.vad_window_ms) {
        params.vad_window_ms = params.vad_last_ms;
    }

    // Decoding sanity
    if (params.max_tokens < 0) {
        params.max_tokens = 0;
    }

    // Filtering sanity
    if (params.dedup_similarity < 0.0f) params.dedup_similarity = 0.0f;
    if (params.dedup_similarity > 1.0f) params.dedup_similarity = 1.0f;

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

    // If user did not specify a device, list all devices and prompt for selection.
    // If stdin is not interactive, fall back to SDL default device (-1).
    if (params.device_index < 0 && params.device_name_substring.empty()) {
        const int chosen = sdl_prompt_for_device_index(/*default_index*/ 0);
        if (chosen >= 0) {
            params.device_index = chosen;
            std::fprintf(stderr, "Using capture device index %d (selected interactively)\n", params.device_index);
        }
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

    streamerbot_ws_client bot;

    std::vector<float> pcm_vad_window;
    std::vector<float> pcm_block;
    std::vector<float> pcm_lang;
    std::string last_sent;

    std::fprintf(stderr, "\nAi-Subtitler started.\n");
    std::fprintf(stderr, "- Capture device index: %d\n", params.device_index);
    std::fprintf(stderr, "- VAD: length_ms=%d check_ms=%d vad_window_ms=%d vad_last_ms=%d vad_thold=%.2f freq_thold=%.1f\n",
        params.length_ms, params.vad_check_ms, params.vad_window_ms, params.vad_last_ms, params.vad_thold, params.freq_thold);
    std::fprintf(stderr, "- Streamer.bot: %s (Action='%s', Arg='%s')\n", params.bot.url.c_str(), params.bot.action_name.c_str(), params.bot.arg_key.c_str());
    std::fprintf(stderr, "Speak normally, then pause briefly to send a block.\n\n");

    if (!params.startup_text.empty()) {
        std::string err;
        if (!bot.connect_and_handshake(params.bot, err)) {
            std::fprintf(stderr, "Streamer.bot connect failed (%s). Will keep running and retry on first transcript.\n", err.c_str());
        } else {
            std::fprintf(stderr, "Connected to Streamer.bot WebSocket: %s\n", params.bot.url.c_str());
            if (!bot.do_action_text(params.bot, params.startup_text, err)) {
                std::fprintf(stderr, "Streamer.bot DoAction startup-text failed (%s).\n", err.c_str());
            } else {
                std::fprintf(stderr, "Streamer.bot startup-text sent.\n");
            }
            bot.close();
        }
    }

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
        if (t_diff < params.vad_check_ms) {
            const int32_t sleep_ms = std::min<int32_t>(50, std::max<int32_t>(1, params.vad_check_ms / 3));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            continue;
        }

        audio.get(params.vad_window_ms, pcm_vad_window);
        if (pcm_vad_window.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        // In whisper.cpp, vad_simple() returns true when the last part of the window is relatively silent.
        if (!::vad_simple(pcm_vad_window, WHISPER_SAMPLE_RATE, params.vad_last_ms, params.vad_thold, params.freq_thold, false)) {
            t_last = t_now;
            continue;
        }

        audio.get(params.length_ms, pcm_block);
        if (pcm_block.size() < (size_t) (WHISPER_SAMPLE_RATE * 0.5)) {
            t_last = t_now;
            continue;
        }

        // Fast-mode guard: keyboard clicks / near-silence can trigger VAD and cause hallucinations like "thank you".
        // If the block has very low activity, drop it and clear the buffer so we don't retrigger on the same click.
        if (params.fast) {
            const float frac = audio_activity_fraction(pcm_block, /*abs_thold=*/0.01f);
            if (frac < 0.01f) {
                audio.clear();
                t_last = t_now;
                continue;
            }

            // Crucial: prevent overlap-repeat spam by discarding the already-snapshotted audio.
            // This keeps any new speech during whisper inference for the next iteration.
            audio.clear();
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.print_special = false;
        wparams.print_timestamps = false;
        wparams.no_timestamps = params.fast ? true : false;
        wparams.suppress_blank = true;
        wparams.suppress_nst = params.fast ? true : false;
        wparams.translate = params.translate;
        wparams.single_segment = params.fast ? true : false;
        wparams.max_tokens = params.max_tokens;
        wparams.no_context = params.fast ? true : false;
        if (params.fast) {
            // Greedy decoding: minimize extra sampling work.
            wparams.greedy.best_of = 1;
        }
        // Language selection:
        // - If user asked for auto, enable built-in whisper language detection.
        // - If language is English (default) AND model is multilingual, auto-fallback to French when French is clearly more likely.
        std::string effective_language = params.language;
        if (params.language == "auto") {
            wparams.detect_language = true;
            wparams.language = "auto";
        } else {
            wparams.detect_language = false;
            if (params.language == "en" && whisper_is_multilingual(ctx)) {
                if (params.fast) {
                    // Keep fast mode snappy: detect from a short tail instead of the full block.
                    const int32_t tail_ms = std::min<int32_t>(1500, std::max<int32_t>(500, params.length_ms));
                    const size_t tail_samples = (size_t) (tail_ms * WHISPER_SAMPLE_RATE / 1000);
                    pcm_lang.clear();
                    if (pcm_block.size() > tail_samples) {
                        pcm_lang.insert(pcm_lang.end(), pcm_block.end() - tail_samples, pcm_block.end());
                    } else {
                        pcm_lang = pcm_block;
                    }
                    effective_language = pick_language_en_fallback_fr(ctx, pcm_lang, params.threads);
                } else {
                    effective_language = pick_language_en_fallback_fr(ctx, pcm_block, params.threads);
                }
            }
            wparams.language = effective_language.c_str();
        }
        wparams.n_threads = params.threads;
        wparams.audio_ctx = 0;

        if (whisper_full(ctx, wparams, pcm_block.data(), pcm_block.size()) != 0) {
            std::fprintf(stderr, "whisper_full failed\n");
            t_last = t_now;
            continue;
        }

        std::string text;
        const int n_segments = whisper_full_n_segments(ctx);
        float max_no_speech_prob = 0.0f;
        for (int i = 0; i < n_segments; ++i) {
            max_no_speech_prob = std::max(max_no_speech_prob, whisper_full_get_segment_no_speech_prob(ctx, i));
            const char * seg = whisper_full_get_segment_text(ctx, i);
            if (seg) text += seg;
        }

        text = trim_and_collapse_ws(text);
        if (text.empty()) {
            t_last = t_now;
            continue;
        }

        // whisper.cpp can emit this special token when the audio block is effectively silence.
        // Don't send it to Streamer.bot.
        if (text == "[BLANK_AUDIO]") {
            t_last = t_now;
            continue;
        }

        // Tiny models can hallucinate short polite phrases after an utterance or during near-silence.
        // Only suppress this in fast mode AND only when whisper itself says it's likely no-speech.
        if (params.fast && is_exact_thank_you(text) && max_no_speech_prob >= 0.80f) {
            t_last = t_now;
            continue;
        }

        // De-dupe: skip very similar repeats (common with sliding windows).
        if (!last_sent.empty()) {
            // Strong de-dupe for the common suffix-repeat artifact:
            //   "hello this is a test" -> "this is a test" -> "is a test" -> ...
            if (is_suffix_repeat_by_words(last_sent, text)) {
                t_last = t_now;
                continue;
            }
            const float sim = ::similarity(last_sent, text);
            if (sim >= params.dedup_similarity) {
                t_last = t_now;
                continue;
            }
        }

        std::printf("[%d] %s\n", iter++, text.c_str());
        std::fflush(stdout);

        // Send to Streamer.bot (best-effort)
        {
            std::string err;
            // Keep the connection short-lived to avoid servers that close idle/unread sockets.
            // This is sentence-level traffic, so reconnect overhead is acceptable.
            if (!bot.connect_and_handshake(params.bot, err)) {
                std::fprintf(stderr, "Streamer.bot connect failed (%s).\n", err.c_str());
            } else {
                if (!bot.do_action_text(params.bot, text, err)) {
                    std::fprintf(stderr, "DoAction failed (%s).\n", err.c_str());
                }
            }
            bot.close();
        }

        last_sent = text;

        t_last = t_now;
    }

    audio.pause();
    whisper_print_timings(ctx);
    whisper_free(ctx);
    return 0;
}

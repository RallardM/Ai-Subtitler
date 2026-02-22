#include "streamerbot_ws_client.h"

#include "common-sdl.h"
#include "common.h"
#include "common-whisper.h"

#include "ggml-backend.h"
#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <conio.h>
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
    bool debug_thankyou = false;
    float dedup_similarity = 0.90f;

    // output / diagnostics
    bool quiet = false;                    // suppress most non-transcript console output
    std::string suspect_log_path;          // JSONL file path; written only on suspect outputs
    std::string suspect_dump_dir;          // directory; write WAV dumps only on suspect outputs

    // output filtering
    bool suppress_lone_you = false;        // suppress exact single-word "you" (common mis-transcription on noise)

    // voice gate (Silero VAD via whisper.cpp)
    bool voice_gate = true;
    bool debug_voice_gate = false;
    bool debug_voice_stop_ms = false;
    bool trace_voice_gate = false;
    bool trace_voice_gate_status = false;
    std::string test_voice_gate_file;
    std::string vad_model;
    int32_t voice_stop_ms = 930;
    int32_t min_voice_ms = 600;
    float vad_voice_threshold = 0.72f;
};

#if defined(_WIN32)
static std::atomic<bool> g_ctrl_c_requested{false};

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_ctrl_c_requested.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

static void install_ctrl_c_handler_best_effort() {
    g_ctrl_c_requested.store(false);
    (void) SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

static void uninstall_ctrl_c_handler_best_effort() {
    (void) SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
}

static bool poll_key(int & ch) {
    if (!_kbhit()) return false;
    ch = _getch();
    // swallow extended key prefix if present
    if (ch == 0 || ch == 224) {
        if (_kbhit()) (void) _getch();
        return false;
    }
    return true;
}
#endif

static std::string json_escape(const std::string & s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Keep JSONL ASCII-only to avoid encoding issues when viewed/parsed in Windows shells.
                // This is diagnostic output, so readability > perfect Unicode round-tripping.
                if (ch < 0x20 || ch >= 0x80) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) ch);
                    out += buf;
                } else {
                    out.push_back((char) ch);
                }
                break;
        }
    }
    return out;
}

static std::string sanitize_for_filename(const std::string & s, const size_t max_len) {
    std::string out;
    out.reserve(std::min(max_len, s.size()));
    for (unsigned char ch : s) {
        if (out.size() >= max_len) break;
        if (std::isalnum(ch)) {
            out.push_back((char) std::tolower(ch));
        } else if (ch == ' ' || ch == '-' || ch == '_') {
            out.push_back('-');
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
    if (out.empty()) out = "suspect";
    return out;
}

static bool write_wav_16le_mono(const std::filesystem::path & path, const std::vector<float> & pcm, const int sample_rate) {
    // Minimal WAV writer: 16-bit PCM, mono.
    if (pcm.empty() || sample_rate <= 0) {
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    auto write_u16 = [&](uint16_t v) {
        out.put((char) (v & 0xff));
        out.put((char) ((v >> 8) & 0xff));
    };
    auto write_u32 = [&](uint32_t v) {
        out.put((char) (v & 0xff));
        out.put((char) ((v >> 8) & 0xff));
        out.put((char) ((v >> 16) & 0xff));
        out.put((char) ((v >> 24) & 0xff));
    };

    const uint16_t num_channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = (uint32_t) sample_rate * num_channels * (bits_per_sample / 8);
    const uint16_t block_align = (uint16_t) (num_channels * (bits_per_sample / 8));
    const uint32_t data_bytes = (uint32_t) (pcm.size() * sizeof(int16_t));
    const uint32_t riff_size = 4 + (8 + 16) + (8 + data_bytes);

    out.write("RIFF", 4);
    write_u32(riff_size);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    write_u32(16);
    write_u16(1);
    write_u16(num_channels);
    write_u32((uint32_t) sample_rate);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(bits_per_sample);

    out.write("data", 4);
    write_u32(data_bytes);

    for (float v : pcm) {
        const float clamped = std::max(-1.0f, std::min(1.0f, v));
        const int32_t s = (int32_t) std::lround(clamped * 32767.0f);
        const int16_t s16 = (int16_t) std::max(-32768, std::min(32767, s));
        write_u16((uint16_t) s16);
    }

    return (bool) out;
}

static void append_text_file_best_effort(const std::string & path, const std::string & line) {
    if (path.empty()) return;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;
    out.write(line.data(), (std::streamsize) line.size());
    out.put('\n');
}

static void print_voice_gate_trace(FILE * f, const char * tag, const int64_t t_ms, const int64_t voice_ms, const int32_t block_ms) {
    if (!f || !tag) return;
    if (voice_ms >= 0 && block_ms >= 0) {
        std::fprintf(f, "[VG] %s t=%lldms voice=%lldms block=%dms\n", tag, (long long) t_ms, (long long) voice_ms, (int) block_ms);
    } else {
        std::fprintf(f, "[VG] %s t=%lldms\n", tag, (long long) t_ms);
    }
    std::fflush(f);
}

static int64_t ms_since(const std::chrono::high_resolution_clock::time_point & t0,
                        const std::chrono::high_resolution_clock::time_point & t1) {
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

static void print_voice_gate_status(FILE * f,
                                   const int64_t t_ms,
                                   const bool in_voice,
                                   const bool voice_present,
                                   const int segs_n,
                                   const int64_t silent_ms,
                                   const int64_t voice_ms,
                                   const float window_rms,
                                   const size_t window_samples) {
    if (!f) return;
    std::fprintf(f,
        "[VG] STATUS t=%lldms in_voice=%d voice_present=%d segs=%d silent=%lldms voice=%lldms win_rms=%.4f win_n=%zu\n",
        (long long) t_ms,
        in_voice ? 1 : 0,
        voice_present ? 1 : 0,
        segs_n,
        (long long) silent_ms,
        (long long) voice_ms,
        window_rms,
        window_samples);
    std::fflush(f);
}

static int run_test_voice_gate_on_file(const app_params & params) {
    if (params.test_voice_gate_file.empty()) {
        std::fprintf(stderr, "error: --test-voice-gate requires a file path\n");
        return 1;
    }
    if (params.vad_model.empty()) {
        std::fprintf(stderr, "error: --test-voice-gate requires a VAD model. Expected ./models/ggml-silero-v6.2.0.bin\n");
        std::fprintf(stderr, "Hint: run .\\download-vad.cmd\n");
        return 1;
    }

    std::vector<float> pcm;
    std::vector<std::vector<float>> pcm_stereo;
    if (!read_audio_data(params.test_voice_gate_file, pcm, pcm_stereo, /*stereo*/false)) {
        std::fprintf(stderr, "error: failed to read audio file: %s\n", params.test_voice_gate_file.c_str());
        return 2;
    }
    if (pcm.empty()) {
        std::fprintf(stderr, "error: audio file is empty: %s\n", params.test_voice_gate_file.c_str());
        return 2;
    }

    whisper_vad_context_params vcp = whisper_vad_default_context_params();
    vcp.n_threads = std::max(1, params.threads);
    vcp.use_gpu = false;
    vcp.gpu_device = 0;

    whisper_vad_context * vctx = whisper_vad_init_from_file_with_params(params.vad_model.c_str(), vcp);
    if (!vctx) {
        std::fprintf(stderr, "error: failed to init VAD model: %s\n", params.vad_model.c_str());
        std::fprintf(stderr, "Hint: run .\\download-vad.cmd\n");
        return 3;
    }

    whisper_vad_params vadp = whisper_vad_default_params();
    vadp.threshold = params.vad_voice_threshold;
    vadp.min_speech_duration_ms = 250;
    vadp.min_silence_duration_ms = 200;
    vadp.max_speech_duration_s = 30.0f;
    vadp.speech_pad_ms = 100;
    vadp.samples_overlap = 0.0f;

    const int64_t total_samples = (int64_t) pcm.size();
    const int64_t total_ms = (int64_t) ((1000.0 * (double) total_samples) / (double) WHISPER_SAMPLE_RATE);

    std::fprintf(stderr, "\nVoice gate OFFLINE test\n");
    std::fprintf(stderr, "- audio: %s\n", params.test_voice_gate_file.c_str());
    std::fprintf(stderr, "- duration: %lldms (samples=%lld @ %dHz)\n", (long long) total_ms, (long long) total_samples, WHISPER_SAMPLE_RATE);
    std::fprintf(stderr, "- vad_model: %s\n", params.vad_model.c_str());
    std::fprintf(stderr, "- stop=%dms min=%dms thold=%.2f check=%dms window=%dms length=%dms\n\n",
        params.voice_stop_ms,
        params.min_voice_ms,
        params.vad_voice_threshold,
        params.vad_check_ms,
        params.vad_window_ms,
        params.length_ms);

    bool in_voice = false;
    bool silence_started = false;
    int64_t t_voice_start_ms = 0;
    int64_t t_last_voice_ms = 0;
    int flushes = 0;

    auto eval_at_ms = [&](const int64_t t_ms) {
        const int64_t end_sample = std::min<int64_t>(total_samples, (t_ms * WHISPER_SAMPLE_RATE) / 1000);
        const int64_t win_samples = (int64_t) ((int64_t) params.vad_window_ms * WHISPER_SAMPLE_RATE) / 1000;
        const int64_t start_sample = std::max<int64_t>(0, end_sample - win_samples);

        std::vector<float> window;
        if (end_sample > start_sample) {
            window.insert(window.end(), pcm.begin() + (size_t) start_sample, pcm.begin() + (size_t) end_sample);
        }

        bool voice_present = false;
        if (!window.empty()) {
            whisper_vad_segments * segs = whisper_vad_segments_from_samples(vctx, vadp, window.data(), (int) window.size());
            voice_present = segs && whisper_vad_segments_n_segments(segs) > 0;
            if (segs) whisper_vad_free_segments(segs);
        }

        if (voice_present) {
            if (!in_voice) {
                in_voice = true;
                silence_started = false;
                t_voice_start_ms = t_ms;
                print_voice_gate_trace(stdout, "VOICE_START", t_ms, -1, -1);
            }
            t_last_voice_ms = t_ms;
            return;
        }

        if (!in_voice) {
            return;
        }

        if (!silence_started) {
            silence_started = true;
            print_voice_gate_trace(stdout, "VOICE_END", t_ms, -1, -1);
        }

        const int64_t silent_ms = t_ms - t_last_voice_ms;
        if (silent_ms < params.voice_stop_ms) {
            return;
        }

        const int64_t voice_ms = t_last_voice_ms - t_voice_start_ms;
        if (voice_ms >= params.min_voice_ms) {
            int32_t block_ms = (int32_t) std::min<int64_t>((int64_t) params.length_ms, t_ms - t_voice_start_ms);
            block_ms = std::max<int32_t>(0, block_ms);
            ++flushes;
            print_voice_gate_trace(stdout, "FLUSH", t_ms, voice_ms, block_ms);
        } else {
            print_voice_gate_trace(stdout, "DROP_SHORT", t_ms, voice_ms, 0);
        }

        in_voice = false;
        silence_started = false;
    };

    // Main scan over the file
    const int64_t step_ms = std::max<int32_t>(50, params.vad_check_ms);
    for (int64_t t_ms = 0; t_ms <= total_ms; t_ms += step_ms) {
        eval_at_ms(t_ms);
    }

    // Ensure any trailing utterance flushes by simulating extra silence.
    for (int64_t t_ms = total_ms + step_ms; t_ms <= total_ms + params.voice_stop_ms + step_ms; t_ms += step_ms) {
        eval_at_ms(t_ms);
    }

    whisper_vad_free(vctx);
    std::fprintf(stderr, "\nVoice gate OFFLINE test complete: flushes=%d\n", flushes);
    return 0;
}

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

static std::string wrap_text_wordwise_cols(const std::string & s, const size_t cols) {
    if (cols == 0 || s.size() <= cols) {
        return s;
    }

    // Assumes input already has collapsed whitespace (single spaces).
    std::string out;
    out.reserve(s.size() + s.size() / cols + 8);

    size_t i = 0;
    size_t line_len = 0;

    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }

        const size_t word_start = i;
        while (i < s.size() && s[i] != ' ') {
            ++i;
        }
        const size_t word_len = i - word_start;

        if (out.empty() || line_len == 0) {
            out.append(s, word_start, word_len);
            line_len = word_len;
            continue;
        }

        // Add word on current line if it fits; otherwise wrap to next line.
        if (line_len + 1 + word_len <= cols) {
            out.push_back(' ');
            out.append(s, word_start, word_len);
            line_len += 1 + word_len;
        } else {
            out.push_back('\n');
            out.append(s, word_start, word_len);
            line_len = word_len;
        }
    }

    return out;
}

static double clamp_double(const double v, const double lo, const double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

struct whisper_log_filter_cfg {
    bool suppress_all = false;
    bool suppress_vad = false;
};

static void whisper_log_filter_cb(ggml_log_level /*level*/, const char * text, void * user_data) {
    if (!text) return;
    const auto * cfg = (const whisper_log_filter_cfg *) user_data;
    if (cfg && cfg->suppress_all) {
        return;
    }
    if (cfg && cfg->suppress_vad) {
        if (std::strstr(text, "whisper_vad_") != nullptr) {
            return;
        }
    }
    std::fputs(text, stderr);
}

struct streamerbot_send_item {
    std::string text;
    size_t raw_len = 0; // original transcript length (including spaces), excluding any wrapping newlines
};

class streamerbot_sender {
public:
    explicit streamerbot_sender(streamerbot_ws_config cfg)
        : m_cfg(std::move(cfg))
        , m_thread([this]() { this->run(); }) {
    }

    ~streamerbot_sender() {
        stop_and_join(/*drain*/true);
    }

    streamerbot_sender(const streamerbot_sender &) = delete;
    streamerbot_sender & operator=(const streamerbot_sender &) = delete;

    void enqueue(streamerbot_send_item item) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_stop) {
                return;
            }
            m_q.push_back(std::move(item));
        }
        m_cv.notify_one();
    }

    void stop_and_join(const bool drain) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_stopped) {
                // Already joined.
                return;
            }
            m_stop = true;
            if (!drain) {
                m_q.clear();
            }
        }
        m_cv.notify_one();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_stopped = true;
        }
    }

private:
    static std::chrono::milliseconds compute_delay_ms(const size_t raw_len, const size_t backlog_remaining) {
        // Length-based reading delay, clamped to [2s, 4s].
        // When the queue is backing up, speed up slightly (but never below 2s).
        constexpr double k_min_s = 2.0;
        constexpr double k_max_s = 4.0;
        constexpr double k_base_chars_per_s = 16.0;
        constexpr size_t k_soft_backlog = 5;

        double speedup = 1.0;
        if (backlog_remaining > k_soft_backlog) {
            // +25% chars/s per extra queued message, capped.
            const double extra = 0.25 * (double) (backlog_remaining - k_soft_backlog);
            speedup = 1.0 + std::min(2.0, extra); // cap at 3x
        }

        const double cps = k_base_chars_per_s * speedup;
        const double delay_s = clamp_double((double) raw_len / cps, k_min_s, k_max_s);
        const int ms = (int) std::llround(delay_s * 1000.0);
        return std::chrono::milliseconds(std::max(0, ms));
    }

    void run() {
        streamerbot_ws_client bot;

        while (true) {
            streamerbot_send_item item;
            size_t backlog_remaining = 0;

            {
                std::unique_lock<std::mutex> lock(m_mu);
                m_cv.wait(lock, [&]() { return m_stop || !m_q.empty(); });

                if (m_q.empty()) {
                    if (m_stop) {
                        break;
                    }
                    continue;
                }

                item = std::move(m_q.front());
                m_q.pop_front();
                backlog_remaining = m_q.size();
            }

            // Best-effort send (same behavior as the main loop used to have).
            {
                std::string err;
                if (!bot.connect_and_handshake(m_cfg, err)) {
                    std::fprintf(stderr, "Streamer.bot connect failed (%s).\n", err.c_str());
                } else {
                    if (!bot.do_action_text(m_cfg, item.text, err)) {
                        std::fprintf(stderr, "DoAction failed (%s).\n", err.c_str());
                    }
                }
                bot.close();
            }

            // If stopping, drain quickly (no additional delay).
            {
                std::lock_guard<std::mutex> lock(m_mu);
                if (m_stop) {
                    if (m_q.empty()) {
                        break;
                    }
                    continue;
                }
            }

            std::this_thread::sleep_for(compute_delay_ms(item.raw_len, backlog_remaining));
        }
    }

private:
    streamerbot_ws_config m_cfg;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<streamerbot_send_item> m_q;
    bool m_stop = false;
    bool m_stopped = false;
    std::thread m_thread;
};

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

static bool is_nonneg_int_str(const std::string & s) {
    if (s.empty()) return false;
    for (unsigned char ch : s) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
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

static float audio_rms(const std::vector<float> & pcm) {
    if (pcm.empty()) return 0.0f;
    double sumsq = 0.0;
    for (float v : pcm) {
        sumsq += (double) v * (double) v;
    }
    return (float) std::sqrt(sumsq / (double) pcm.size());
}

static bool is_exact_you(const std::string & s) {
    const auto w = split_words_lower_ascii(s);
    return w.size() == 1 && w[0] == "you";
}

static bool is_short_garbage_like(const std::string & s) {
    // Heuristic for weird junk output like "ΓÖ¬" that can appear on near-silence.
    // Keep this intentionally conservative to avoid hiding legitimate non-English text.
    if (s.size() > 8) return false;

    const auto w = split_words_lower_ascii(s);
    if (w.empty()) {
        return true;
    }

    for (unsigned char ch : s) {
        if (ch >= 0x80) {
            return true;
        }
    }

    return false;
}

static bool is_exact_thank_you(const std::string & s) {
    const auto w = split_words_lower_ascii(s);
    if (w.size() == 2 && w[0] == "thank" && w[1] == "you") return true;
    if (w.size() == 1 && w[0] == "thankyou") return true;
    return false;
}

static std::string maybe_dump_suspect_wav(const app_params & params,
                                         const int64_t t_ms,
                                         const int iter,
                                         const std::string & text,
                                         const std::vector<float> & pcm) {
    if (params.suspect_dump_dir.empty() || pcm.empty()) {
        return {};
    }

    try {
        std::filesystem::path dir(params.suspect_dump_dir);
        std::filesystem::create_directories(dir);

        std::ostringstream name;
        name << "suspect-" << std::setw(6) << std::setfill('0') << iter;
        name << "-t" << t_ms << "ms-";
        name << sanitize_for_filename(text, /*max_len*/48);
        name << ".wav";

        const auto out_path = dir / name.str();
        if (!write_wav_16le_mono(out_path, pcm, WHISPER_SAMPLE_RATE)) {
            return {};
        }
        return out_path.filename().string();
    } catch (...) {
        return {};
    }
}

static void maybe_log_suspect_event_jsonl(const app_params & params,
                                         const int64_t t_ms,
                                         const int iter,
                                         const bool gated_block,
                                         const int32_t block_ms,
                                         const int64_t voice_ms,
                                         const int64_t silent_ms,
                                         const int64_t trim_ms,
                                         const size_t pcm_n,
                                         const float block_frac,
                                         const float block_rms,
                                         const std::string & effective_language,
                                         const float max_no_speech_prob,
                                         const int n_segments,
                                         const std::string & text,
                                         const bool suppressed,
                                         const char * reason,
                                         const std::string & wav_file) {
    if (params.suspect_log_path.empty()) {
        return;
    }

    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << '{';
    ss << "\"t_ms\":" << t_ms;
    ss << ",\"iter\":" << iter;
    ss << ",\"mode\":\"" << (gated_block ? "voice_gate" : "simple_vad") << "\"";
    ss << ",\"block_ms\":" << block_ms;
    ss << ",\"voice_ms\":" << voice_ms;
    ss << ",\"silent_ms\":" << silent_ms;
    ss << ",\"trim_ms\":" << trim_ms;
    ss << ",\"pcm_n\":" << pcm_n;
    ss << ",\"block_frac\":" << std::setprecision(5) << block_frac;
    ss << ",\"block_rms\":" << std::setprecision(6) << block_rms;
    ss << ",\"lang\":\"" << json_escape(effective_language) << "\"";
    ss << ",\"max_no_speech\":" << std::setprecision(3) << max_no_speech_prob;
    ss << ",\"n_segments\":" << n_segments;
    ss << ",\"text\":\"" << json_escape(text) << "\"";
    ss << ",\"suppressed\":" << (suppressed ? "true" : "false");
    ss << ",\"reason\":\"" << json_escape(reason ? reason : "") << "\"";
    if (!wav_file.empty()) {
        ss << ",\"wav\":\"" << json_escape(wav_file) << "\"";
    }
    ss << '}';

    append_text_file_best_effort(params.suspect_log_path, ss.str());
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
    std::fprintf(stderr, "Usage: %s [mic_index] [--model <path>] [options]\n\n", exe);
    std::fprintf(stderr, "Whisper:\n");
    std::fprintf(stderr, "  --model <path>            Path to ggml model (optional if ./models contains a known model)\n");
    std::fprintf(stderr, "  --language <auto|en|...>  Spoken language (default: en; auto-fallback to fr when likely)\n");
    std::fprintf(stderr, "  --threads N               Threads (default: cores-1)\n");
    std::fprintf(stderr, "  --translate               Translate to English\n");
    std::fprintf(stderr, "  --no-gpu                  Disable GPU inference\n");
    std::fprintf(stderr, "  --no-flash-attn           Disable flash-attn\n\n");

    std::fprintf(stderr, "Presets:\n");
    std::fprintf(stderr, "  --fast                    Faster, less accurate (shorter blocks, no extra language-detect pass, more aggressive decoding)\n\n");

    std::fprintf(stderr, "Audio/VAD:\n");
    std::fprintf(stderr, "  --list-devices            List capture devices and exit\n");
    std::fprintf(stderr, "  mic_index                 Positional shortcut for mic index (e.g. '%s 0')\n", exe);
    std::fprintf(stderr, "  --mic <N|substring>       Microphone selection shortcut: index (e.g. --mic 0) or name substring (e.g. --mic Samson)\n");
    std::fprintf(stderr, "  --device-index N          Capture device index (SDL2)\n");
    std::fprintf(stderr, "  --device-name <substring> Capture device name substring (preferred)\n");
    std::fprintf(stderr, "                           If neither is provided, the app will list devices and prompt (interactive shells only)\n");
    std::fprintf(stderr, "  --length-ms N             Window length for VAD blocks (default: 30000; fast preset: 6000)\n");
    std::fprintf(stderr, "  --vad-check-ms N          How often to evaluate VAD (default: 2000; fast preset: 150)\n");
    std::fprintf(stderr, "  --vad-window-ms N         Window size used for VAD evaluation (default: 2000; fast preset: 1500)\n");
    std::fprintf(stderr, "  --vad-last-ms N           Trailing tail that must be quiet to flush (default: 1000; fast preset: 650)\n");
    std::fprintf(stderr, "  --vad-thold X             VAD threshold (default: 0.60)\n");
    std::fprintf(stderr, "  --freq-thold X            High-pass cutoff (default: 100.0)\n\n");

    std::fprintf(stderr, "Voice gate (speech vs noise):\n");
    std::fprintf(stderr, "  --no-voice-gate           Disable voice/noise gating and use the simple silence-tail VAD\n");
    std::fprintf(stderr, "  --trace-voice-gate        Print voice gate events (VOICE_START/VOICE_END/FLUSH)\n");
    std::fprintf(stderr, "  --trace-voice-gate-status Print periodic voice gate status lines (very verbose)\n");
    std::fprintf(stderr, "  --vad-model <path>        Path to Silero VAD model (default: ./models/ggml-silero-v6.2.0.bin if present)\n");
    std::fprintf(stderr, "  --voice-stop-ms N         How long voice must be absent before flushing (default: 930)\n");
    std::fprintf(stderr, "  --min-voice-ms N          Minimum voice duration required to send to Whisper (default: 600)\n");
    std::fprintf(stderr, "  --vad-voice-thold X       Silero VAD probability threshold (default: 0.72)\n\n");

    std::fprintf(stderr, "Decoding:\n");
    std::fprintf(stderr, "  --max-tokens N            Max tokens per block (0 = no limit; fast preset: 48)\n\n");

    std::fprintf(stderr, "Streamer.bot:\n");
    std::fprintf(stderr, "  --ws-url ws://127.0.0.1:8080/   WebSocket URL\n");
    std::fprintf(stderr, "  --ws-password <pwd>       Optional WebSocket password\n");
    std::fprintf(stderr, "  --action-name \"AI Subtitler\"   Action to execute\n");
    std::fprintf(stderr, "  --arg-key AiText           Argument key (default: AiText)\n\n");

    std::fprintf(stderr, "Diagnostics:\n");
    std::fprintf(stderr, "  --startup-text <text>      Send a DoAction immediately after start (useful to verify Streamer.bot connectivity)\n\n");
    std::fprintf(stderr, "  --debug-thankyou           Print debug info whenever output is exactly \"Thank you.\" (you can use this to tune filters)\n\n");

    std::fprintf(stderr, "  --quiet                   Suppress most non-transcript console output (keeps stdout as just transcripts)\n");
    std::fprintf(stderr, "  --suspect-log <path>      Append JSONL debug entries only when output looks suspect (thank you / lone you / short junk)\n");
    std::fprintf(stderr, "  --suspect-dump-dir <dir>  Write WAV dumps for the exact audio block that produced a suspect output\n\n");
    std::fprintf(stderr, "  --debug-voice-gate         Debug-only: continuously print DETECT VOICE / DOES NOT DETECT VOICE (no Whisper, no Streamer.bot)\n\n");
    std::fprintf(stderr, "  --debug-voice-stop-ms      Debug-only: voice-gate flush simulator; prints VOICE_START/VOICE_END/FLUSH (no Whisper, no Streamer.bot)\n\n");
    std::fprintf(stderr, "  --test-voice-gate <file>   Offline test: run voice gating on an audio file and print VOICE_* events (no mic, no Whisper)\n\n");

    std::fprintf(stderr, "Output filtering:\n");
    std::fprintf(stderr, "  --dedup-similarity X       Skip very similar repeats (default: 0.90; fast preset: 0.80)\n\n");
    std::fprintf(stderr, "  --suppress-lone-you        Suppress exact single-word 'you' outputs (diagnostic; avoids common noise mis-transcription)\n\n");
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

        // Convenience: allow a leading positional mic index for Windows shortcuts.
        // Example: ai-subtitler-streamerbot.exe 0 --fast
        if (!arg.empty() && arg[0] != '-' && is_nonneg_int_str(arg)) {
            if (p.device_index < 0 && p.device_name_substring.empty()) {
                p.device_index = std::stoi(arg);
                continue;
            }
        }

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
            // A shorter decode window reduces end-to-end delay, but too small can chop long sentences.
            p.length_ms = 6000;
            // Reduce the "wait to flush" latency by checking VAD frequently, but require enough silence tail to avoid mid-thought flushes.
            p.vad_check_ms = 150;
            p.vad_window_ms = 1500;
            p.vad_last_ms = 650;
            // Reduce decoder work.
            p.max_tokens = 48;
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
        } else if (arg == "--debug-thankyou") {
            p.debug_thankyou = true;
        } else if (arg == "--quiet") {
            p.quiet = true;
        } else if (arg == "--suspect-log") {
            p.suspect_log_path = require_value("--suspect-log");
        } else if (arg == "--suspect-dump-dir") {
            p.suspect_dump_dir = require_value("--suspect-dump-dir");
        } else if (arg == "--debug-voice-gate") {
            p.debug_voice_gate = true;
        } else if (arg == "--debug-voice-stop-ms") {
            p.debug_voice_stop_ms = true;
        } else if (arg == "--trace-voice-gate") {
            p.trace_voice_gate = true;
        } else if (arg == "--trace-voice-gate-status") {
            p.trace_voice_gate_status = true;
        } else if (arg == "--no-voice-gate") {
            p.voice_gate = false;
        } else if (arg == "--test-voice-gate") {
            p.test_voice_gate_file = require_value("--test-voice-gate");
        } else if (arg == "--vad-model") {
            p.vad_model = require_value("--vad-model");
        } else if (arg == "--voice-stop-ms") {
            p.voice_stop_ms = std::stoi(require_value("--voice-stop-ms"));
        } else if (arg == "--min-voice-ms") {
            p.min_voice_ms = std::stoi(require_value("--min-voice-ms"));
        } else if (arg == "--vad-voice-thold") {
            p.vad_voice_threshold = std::stof(require_value("--vad-voice-thold"));
        } else if (arg == "--suppress-lone-you") {
            p.suppress_lone_you = true;
        } else if (arg == "--dedup-similarity") {
            p.dedup_similarity = std::stof(require_value("--dedup-similarity"));
        } else {
            std::fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

static std::string pick_default_vad_model_path() {
    const char * candidates[] = {
        "models/ggml-silero-v6.2.0.bin",
    };

    for (const char * rel : candidates) {
        FILE * f = std::fopen(rel, "rb");
        if (f) {
            std::fclose(f);
            return std::string(rel);
        }
    }

    return {};
}

static std::string pick_default_model_path() {
    // For release builds, prefer a local ./models folder next to where the user runs the exe.
    // This mirrors start-ai-subtitler.cmd.
    const char * candidates[] = {
        "models/ggml-tiny.bin",
        "models/ggml-tiny.en.bin",
        "models/ggml-medium.bin",
    };

    for (const char * rel : candidates) {
        FILE * f = std::fopen(rel, "rb");
        if (f) {
            std::fclose(f);
            return std::string(rel);
        }
    }

    return {};
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

    const bool suspect_debug_enabled = !params.suspect_log_path.empty() || !params.suspect_dump_dir.empty();

    const bool voice_gate_requested_by_default_or_cli = params.voice_gate;

    // Offline voice-gate test mode (no mic, no Whisper, no Streamer.bot)
    if (!params.test_voice_gate_file.empty()) {
        // Suppress whisper/ggml logs so output is only our test events.
        whisper_log_filter_cfg log_cfg{};
        log_cfg.suppress_all = true;
        whisper_log_set(whisper_log_filter_cb, &log_cfg);

        // Apply default VAD model probe if user didn't provide --vad-model.
        if (params.vad_model.empty()) {
            params.vad_model = pick_default_vad_model_path();
        }

        return run_test_voice_gate_on_file(params);
    }

    whisper_log_filter_cfg log_cfg{};
    if (params.debug_voice_gate || params.debug_voice_stop_ms) {
        // Debug voice gate should print ONLY our own DETECT/DOES NOT DETECT lines.
        log_cfg.suppress_all = true;
        whisper_log_set(whisper_log_filter_cb, &log_cfg);
    } else if (params.quiet) {
        // Quiet mode: keep console output clean (stdout is transcripts; stderr is mostly silent).
        log_cfg.suppress_all = true;
        whisper_log_set(whisper_log_filter_cb, &log_cfg);
    } else if (params.voice_gate) {
        // Keep normal whisper logs, but suppress Silero VAD spam.
        log_cfg.suppress_vad = true;
        whisper_log_set(whisper_log_filter_cb, &log_cfg);
    }

    // Sanity/clamping to avoid invalid VAD windows.
    params.vad_check_ms = std::max<int32_t>(50, params.vad_check_ms);
    params.vad_window_ms = std::max<int32_t>(200, params.vad_window_ms);
    params.vad_last_ms = std::max<int32_t>(50, params.vad_last_ms);
    // Important: whisper.cpp's vad_simple() requires vad_window_ms > vad_last_ms.
    if (params.vad_window_ms <= params.vad_last_ms) {
        params.vad_window_ms = params.vad_last_ms + 100;
    }

    // Decoding sanity
    if (params.max_tokens < 0) {
        params.max_tokens = 0;
    }

    // Filtering sanity
    if (params.dedup_similarity < 0.0f) params.dedup_similarity = 0.0f;
    if (params.dedup_similarity > 1.0f) params.dedup_similarity = 1.0f;

    // Voice gate sanity
    params.voice_stop_ms = std::max<int32_t>(250, params.voice_stop_ms);
    params.min_voice_ms = std::max<int32_t>(0, params.min_voice_ms);
    if (params.vad_voice_threshold < 0.0f) params.vad_voice_threshold = 0.0f;
    if (params.vad_voice_threshold > 1.0f) params.vad_voice_threshold = 1.0f;

    if (params.list_devices) {
        return sdl_list_devices_only() ? 0 : 2;
    }

    if (params.vad_model.empty()) {
        params.vad_model = pick_default_vad_model_path();
    }

    if (params.model.empty()) {
        params.model = pick_default_model_path();
    }

    // We do not require a Whisper model for debug-only modes.
    if (!params.debug_voice_gate && !params.debug_voice_stop_ms && params.model.empty()) {
        std::fprintf(stderr, "error: --model is required (or place a model under ./models)\n");
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

    // init Silero VAD (used to distinguish speech vs noise/music)
    whisper_vad_context * vctx = nullptr;
    whisper_vad_params vadp = whisper_vad_default_params();
    vadp.threshold = params.vad_voice_threshold;
    // Filter out brief noise and glitches.
    vadp.min_speech_duration_ms = 250;
    vadp.min_silence_duration_ms = 200;
    vadp.max_speech_duration_s = 30.0f;
    vadp.speech_pad_ms = 100;
    vadp.samples_overlap = 0.0f;

    if (params.voice_gate || params.debug_voice_gate || params.debug_voice_stop_ms) {
        if (params.vad_model.empty()) {
            if (params.debug_voice_gate || params.debug_voice_stop_ms) {
                std::fprintf(stderr, "error: voice gate requires a VAD model. Expected ./models/ggml-silero-v6.2.0.bin\n");
                std::fprintf(stderr, "Hint: run .\\download-vad.cmd\n");
                return 1;
            }
            std::fprintf(stderr, "warning: voice gate enabled but no VAD model found (expected ./models/ggml-silero-v6.2.0.bin).\n");
            std::fprintf(stderr, "         Falling back to simple VAD. To enable voice/noise gating, run: .\\download-vad.cmd\n");
            params.voice_gate = false;
        } else {
            whisper_vad_context_params vcp = whisper_vad_default_context_params();
            vcp.n_threads = std::max(1, params.threads);
            vcp.use_gpu = false;
            vcp.gpu_device = 0;
            vctx = whisper_vad_init_from_file_with_params(params.vad_model.c_str(), vcp);
            if (!vctx) {
                if (params.debug_voice_gate || params.debug_voice_stop_ms) {
                    std::fprintf(stderr, "error: failed to init VAD model: %s\n", params.vad_model.c_str());
                    std::fprintf(stderr, "Hint: run .\\download-vad.cmd\n");
                    return 1;
                }
                std::fprintf(stderr, "warning: failed to init VAD model (%s). Falling back to simple VAD.\n", params.vad_model.c_str());
                params.voice_gate = false;
            }
        }
    }

    // Voice gating needs enough ring-buffer history to include both:
    // - the full spoken segment, and
    // - the required trailing no-voice time (voice_stop_ms)
    // IMPORTANT: apply only when Silero voice gate is actually active.
    // Otherwise, voice-gate fallback can accidentally inflate length_ms and cause huge Whisper blocks.
    if (params.voice_gate && vctx) {
        int32_t min_len_ms = 0;
        if (params.fast) {
            // Fast mode: keep enough history for ~10s utterances.
            min_len_ms = std::max<int32_t>(8000, params.voice_stop_ms + 6000 + params.vad_check_ms);
        } else {
            // Conservative default for non-fast mode.
            min_len_ms = std::max<int32_t>(20000, params.voice_stop_ms + 10000);
        }
        if (params.length_ms < min_len_ms) {
            params.length_ms = min_len_ms;
        }
    }

    // init audio capture (reuse whisper.cpp example helper)
    audio_async audio(params.length_ms);
    if (!audio.init(params.device_index, WHISPER_SAMPLE_RATE)) {
        std::fprintf(stderr, "error: audio.init() failed\n");
        if (vctx) whisper_vad_free(vctx);
        return 3;
    }
    audio.resume();

    if (!params.debug_voice_gate && !params.debug_voice_stop_ms) {
        if (!voice_gate_requested_by_default_or_cli) {
            if (!params.quiet) std::fprintf(stderr, "Voice gate: OFF (--no-voice-gate)\n");
        } else if (params.voice_gate && vctx) {
            if (!params.quiet) std::fprintf(stderr,
                "Voice gate: ON (Silero) stop=%dms min=%dms thold=%.2f model=%s\n",
                params.voice_stop_ms,
                params.min_voice_ms,
                params.vad_voice_threshold,
                params.vad_model.c_str());
        } else {
            if (!params.quiet) std::fprintf(stderr,
                "Voice gate: OFF (fallback to simple VAD; missing/failed Silero model). Expected: ./models/ggml-silero-v6.2.0.bin\n");
        }
    }

    // Debug-only voice gate mode: prints only DETECT VOICE / DOES NOT DETECT VOICE.
    if (params.debug_voice_gate) {
#if defined(_WIN32)
        install_ctrl_c_handler_best_effort();
#endif
        // Tighten responsiveness for debugging: smaller analysis window and lower duration thresholds.
        whisper_vad_params vadp_dbg = vadp;
        vadp_dbg.min_speech_duration_ms = 50;
        vadp_dbg.min_silence_duration_ms = 50;
        vadp_dbg.speech_pad_ms = 0;

        std::vector<float> pcm_dbg;
        auto t_last_dbg = std::chrono::high_resolution_clock::now();
        constexpr int32_t k_debug_period_ms = 10;
        constexpr int32_t k_debug_window_ms = 200;

        while (true) {
            if (!sdl_poll_events()) {
                break;
            }

#if defined(_WIN32)
            if (g_ctrl_c_requested.load()) {
                break;
            }

            int ch = 0;
            while (poll_key(ch)) {
                if (ch == '+' || ch == '=') {
                    params.vad_voice_threshold = std::min(1.0f, params.vad_voice_threshold + 0.01f);
                    vadp_dbg.threshold = params.vad_voice_threshold;
                    std::printf("vad_voice_thold=%.2f\n", params.vad_voice_threshold);
                    std::fflush(stdout);
                } else if (ch == '-' || ch == '_') {
                    params.vad_voice_threshold = std::max(0.0f, params.vad_voice_threshold - 0.01f);
                    vadp_dbg.threshold = params.vad_voice_threshold;
                    std::printf("vad_voice_thold=%.2f\n", params.vad_voice_threshold);
                    std::fflush(stdout);
                }
            }
#endif

            const auto t_now = std::chrono::high_resolution_clock::now();
            const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_dbg).count();
            if (t_diff < k_debug_period_ms) {
                const int64_t remaining = (int64_t) k_debug_period_ms - t_diff;
                std::this_thread::sleep_for(std::chrono::milliseconds((int) std::max<int64_t>(1, std::min<int64_t>(5, remaining))));
                continue;
            }

            audio.get(k_debug_window_ms, pcm_dbg);
            bool voice = false;
            if (vctx && !pcm_dbg.empty()) {
                whisper_vad_segments * segs = whisper_vad_segments_from_samples(vctx, vadp_dbg, pcm_dbg.data(), (int) pcm_dbg.size());
                voice = segs && whisper_vad_segments_n_segments(segs) > 0;
                if (segs) whisper_vad_free_segments(segs);
            }

            std::puts(voice ? "DETECT VOICE" : "DOES NOT DETECT VOICE");
            std::fflush(stdout);

            t_last_dbg = t_now;
        }

        #if defined(_WIN32)
            std::printf("FINAL vad_voice_thold=%.2f\n", params.vad_voice_threshold);
            std::fflush(stdout);
            uninstall_ctrl_c_handler_best_effort();
        #endif

        if (vctx) whisper_vad_free(vctx);
        audio.pause();
        return 0;
    }

    // Debug-only voice stop tuning mode: prints VOICE_START/VOICE_END/FLUSH (no Whisper, no Streamer.bot).
    // Hotkeys (Windows):
    //   '+' / '-' adjust --voice-stop-ms by 10ms
    if (params.debug_voice_stop_ms) {
#if defined(_WIN32)
        install_ctrl_c_handler_best_effort();
#endif

        // IMPORTANT: this mode must be able to detect voice on short windows.
        // The normal voice-gate VAD params use min_speech_duration_ms=250, which would never trigger
        // when analyzing a 200ms window. Use debug-tuned params.
        whisper_vad_params vadp_dbg = vadp;
        vadp_dbg.min_speech_duration_ms = 50;
        vadp_dbg.min_silence_duration_ms = 50;
        vadp_dbg.speech_pad_ms = 0;

        std::vector<float> pcm_dbg;
        auto t_last_dbg = std::chrono::high_resolution_clock::now();
        constexpr int32_t k_debug_period_ms = 10;
        constexpr int32_t k_debug_window_ms = 300;

        std::puts("Tune voice-stop-ms: speak, then stop; FLUSH happens after silence >= voice_stop_ms");
        std::puts("Hotkeys: + / - (10ms), q quits (also Ctrl+C)");
        std::puts("[Start speaking]");
        std::fflush(stdout);

        bool have_utterance_dbg = false;
        bool silence_started_dbg = false;
        auto t_utt_start_dbg = std::chrono::high_resolution_clock::now();
        auto t_last_voice_dbg = t_utt_start_dbg;
        const auto t0 = t_utt_start_dbg;

        auto t_last_status = t0;

        while (true) {
            if (!sdl_poll_events()) {
                break;
            }

#if defined(_WIN32)
            if (g_ctrl_c_requested.load()) {
                break;
            }

            int ch = 0;
            while (poll_key(ch)) {
                if (ch == 'q' || ch == 'Q') {
                    g_ctrl_c_requested.store(true);
                    break;
                }
                if (ch == '+' || ch == '=') {
                    params.voice_stop_ms += 10;
                    std::printf("voice_stop_ms=%d\n", params.voice_stop_ms);
                    std::fflush(stdout);
                } else if (ch == '-' || ch == '_') {
                    params.voice_stop_ms = std::max<int32_t>(250, params.voice_stop_ms - 10);
                    std::printf("voice_stop_ms=%d\n", params.voice_stop_ms);
                    std::fflush(stdout);
                }
            }
#endif

            const auto t_now = std::chrono::high_resolution_clock::now();
            const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_dbg).count();
            if (t_diff < k_debug_period_ms) {
                const int64_t remaining = (int64_t) k_debug_period_ms - t_diff;
                std::this_thread::sleep_for(std::chrono::milliseconds((int) std::max<int64_t>(1, std::min<int64_t>(5, remaining))));
                continue;
            }

            audio.get(k_debug_window_ms, pcm_dbg);
            bool voice_present = false;
            if (vctx && !pcm_dbg.empty()) {
                whisper_vad_segments * segs = whisper_vad_segments_from_samples(vctx, vadp_dbg, pcm_dbg.data(), (int) pcm_dbg.size());
                voice_present = segs && whisper_vad_segments_n_segments(segs) > 0;
                if (segs) whisper_vad_free_segments(segs);
            }

            if (voice_present) {
                if (!have_utterance_dbg) {
                    have_utterance_dbg = true;
                    silence_started_dbg = false;
                    t_utt_start_dbg = t_now;
                    t_last_voice_dbg = t_now;
                    std::printf("VOICE_START t=%lldms\n", (long long) ms_since(t0, t_now));
                    std::fflush(stdout);
                }
                t_last_voice_dbg = t_now;
            } else {
                if (have_utterance_dbg) {
                    if (!silence_started_dbg) {
                        silence_started_dbg = true;
                        std::printf("VOICE_END t=%lldms\n", (long long) ms_since(t0, t_now));
                        std::fflush(stdout);
                    }
                    const int64_t silent_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_voice_dbg).count();
                    if (silent_ms >= params.voice_stop_ms) {
                        const int64_t voice_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_last_voice_dbg - t_utt_start_dbg).count();
                        std::printf("FLUSH voice_ms=%lld silent_ms=%lld voice_stop_ms=%d\n",
                            (long long) voice_ms,
                            (long long) silent_ms,
                            params.voice_stop_ms);
                        std::fflush(stdout);

                        // Reset for next utterance.
                        have_utterance_dbg = false;
                        silence_started_dbg = false;
                    }
                }
            }

            const auto status_diff_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_status).count();
            if (status_diff_ms >= 250) {
                const int64_t silent_ms = have_utterance_dbg ? (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_voice_dbg).count() : -1;
                std::printf("[TUNE] voice_present=%d have_utt=%d silent_ms=%lld voice_stop_ms=%d vad_voice_thold=%.2f rms=%.4f\n",
                    voice_present ? 1 : 0,
                    have_utterance_dbg ? 1 : 0,
                    (long long) silent_ms,
                    params.voice_stop_ms,
                    params.vad_voice_threshold,
                    audio_rms(pcm_dbg));
                std::fflush(stdout);
                t_last_status = t_now;
            }

            t_last_dbg = t_now;
        }

#if defined(_WIN32)
        std::printf("FINAL voice_stop_ms=%d\n", params.voice_stop_ms);
        std::fflush(stdout);
        uninstall_ctrl_c_handler_best_effort();
#endif

        if (vctx) whisper_vad_free(vctx);
        audio.pause();
        return 0;
    }

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
    streamerbot_sender bot_sender(params.bot);

    std::vector<float> pcm_vad_window;
    std::vector<float> pcm_block;
    std::vector<float> pcm_lang;
    std::string last_sent;

    bool in_voice = false;
    auto t_voice_start = std::chrono::high_resolution_clock::now();
    auto t_last_voice  = t_voice_start;
    bool trace_silence_started = false;
    const auto t_trace0 = std::chrono::high_resolution_clock::now();
    auto t_last_vg_status = t_trace0;

    if (!params.quiet) {
        std::fprintf(stderr, "\nAi-Subtitler started.\n");
        std::fprintf(stderr, "- Capture device index: %d\n", params.device_index);
        std::fprintf(stderr, "- VAD: length_ms=%d check_ms=%d vad_window_ms=%d vad_last_ms=%d vad_thold=%.2f freq_thold=%.1f\n",
            params.length_ms, params.vad_check_ms, params.vad_window_ms, params.vad_last_ms, params.vad_thold, params.freq_thold);
        std::fprintf(stderr, "- Streamer.bot: %s (Action='%s', Arg='%s')\n", params.bot.url.c_str(), params.bot.action_name.c_str(), params.bot.arg_key.c_str());
        if (suspect_debug_enabled) {
            std::fprintf(stderr, "- Suspect debug: log='%s' dump_dir='%s'\n",
                params.suspect_log_path.c_str(),
                params.suspect_dump_dir.c_str());
        }
        std::fprintf(stderr, "Speak normally, then pause briefly to send a block.\n\n");
    }

    if (!params.startup_text.empty()) {
        std::string err;
        if (!bot.connect_and_handshake(params.bot, err)) {
            if (!params.quiet) {
                std::fprintf(stderr, "Streamer.bot connect failed (%s). Will keep running and retry on first transcript.\n", err.c_str());
            }
        } else {
            if (!params.quiet) {
                std::fprintf(stderr, "Connected to Streamer.bot WebSocket: %s\n", params.bot.url.c_str());
            }
            if (!bot.do_action_text(params.bot, params.startup_text, err)) {
                std::fprintf(stderr, "Streamer.bot DoAction startup-text failed (%s).\n", err.c_str());
            } else {
                if (!params.quiet) {
                    std::fprintf(stderr, "Streamer.bot startup-text sent.\n");
                }
            }
            bot.close();
        }
    }

    if (!params.quiet) {
        std::puts("[Start speaking]");
        std::fflush(stdout);
    }

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

        bool have_pcm_block = false;
        bool gated_block = false;
        int32_t dbg_block_ms = -1;
        int64_t dbg_voice_ms = -1;
        int64_t dbg_silent_ms = -1;
        int64_t dbg_trim_ms = -1;

        // Voice gate mode: only run Whisper when speech has ended for long enough.
        if (params.voice_gate && vctx) {
            bool voice_present = false;
            int segs_n = 0;
            whisper_vad_segments * segs = whisper_vad_segments_from_samples(vctx, vadp, pcm_vad_window.data(), (int) pcm_vad_window.size());
            segs_n = segs ? whisper_vad_segments_n_segments(segs) : 0;
            voice_present = segs_n > 0;
            if (segs) whisper_vad_free_segments(segs);

            if (!params.quiet && params.trace_voice_gate && params.trace_voice_gate_status) {
                const auto status_diff_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_vg_status).count();
                if (status_diff_ms >= 1000) {
                    const int64_t silent_ms = in_voice ? (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_voice).count() : -1;
                    const int64_t voice_ms  = in_voice ? (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_last_voice - t_voice_start).count() : -1;
                    print_voice_gate_status(stderr,
                        ms_since(t_trace0, t_now),
                        in_voice,
                        voice_present,
                        segs_n,
                        silent_ms,
                        voice_ms,
                        audio_rms(pcm_vad_window),
                        pcm_vad_window.size());
                    t_last_vg_status = t_now;
                }
            }

            if (voice_present) {
                if (!in_voice) {
                    in_voice = true;
                    t_voice_start = t_now;
                    if (!params.quiet && params.trace_voice_gate) {
                        trace_silence_started = false;
                        print_voice_gate_trace(stderr, "VOICE_START", ms_since(t_trace0, t_now), -1, -1);
                    }
                }
                t_last_voice = t_now;
                t_last = t_now;
                continue;
            }

            if (in_voice) {
                if (!params.quiet && params.trace_voice_gate && !trace_silence_started) {
                    trace_silence_started = true;
                    print_voice_gate_trace(stderr, "VOICE_END", ms_since(t_trace0, t_now), -1, -1);
                }
                const auto silent_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last_voice).count();
                if (silent_ms >= params.voice_stop_ms) {
                    const auto voice_ms = (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_last_voice - t_voice_start).count();
                    dbg_silent_ms = silent_ms;
                    dbg_voice_ms = voice_ms;

                    if (voice_ms >= params.min_voice_ms) {
                        int32_t block_ms = (int32_t) std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_voice_start).count();
                        block_ms = std::max<int32_t>(0, std::min<int32_t>(block_ms, params.length_ms));
                        dbg_block_ms = block_ms;

                        if (!params.quiet && params.trace_voice_gate) {
                            print_voice_gate_trace(stderr, "FLUSH", ms_since(t_trace0, t_now), voice_ms, block_ms);
                        }

                        audio.get(block_ms, pcm_block);

                        // IMPORTANT: in voice-gate mode we intentionally wait for `voice_stop_ms` of silence.
                        // The `block_ms` above includes that trailing silence, which can cause tiny models to hallucinate
                        // short outputs like "Thank you" / "you" / junk glyphs on the silent tail.
                        // We cannot fix this by shrinking block_ms (audio.get(ms) returns the most recent ms, which would
                        // chop the *start* of speech). Instead, trim the silence from the end of the captured block.
                        {
                            constexpr int32_t k_keep_tail_ms = 200;
                            const int64_t trim_ms = std::max<int64_t>(0, silent_ms - k_keep_tail_ms);
                            dbg_trim_ms = trim_ms;
                            const size_t trim_samples = (size_t) ((trim_ms * WHISPER_SAMPLE_RATE) / 1000);
                            if (trim_samples > 0 && trim_samples < pcm_block.size()) {
                                pcm_block.resize(pcm_block.size() - trim_samples);
                            } else if (trim_samples >= pcm_block.size()) {
                                pcm_block.clear();
                            }
                        }

                        if (!params.quiet && params.trace_voice_gate) {
                            std::fprintf(stderr,
                                "[VG] FLUSH_AUDIO silent=%lldms block_ms=%d pcm_n=%zu rms=%.4f\n",
                                (long long) silent_ms,
                                (int) block_ms,
                                pcm_block.size(),
                                audio_rms(pcm_block));
                            std::fflush(stderr);
                        }
                        if (pcm_block.size() >= (size_t) (WHISPER_SAMPLE_RATE * 0.5)) {
                            have_pcm_block = true;
                            gated_block = true;
                        } else {
                            if (!params.quiet && params.trace_voice_gate) {
                                std::fprintf(stderr, "[VG] DROP_TOO_SHORT pcm_n=%zu (need >= %.0f)\n",
                                    pcm_block.size(),
                                    (double) (WHISPER_SAMPLE_RATE * 0.5));
                                std::fflush(stderr);
                            }
                            audio.clear();
                            in_voice = false;
                            t_last = t_now;
                            continue;
                        }
                    } else {
                        // Too short: likely a click / noise burst.
                        if (!params.quiet && params.trace_voice_gate) {
                            print_voice_gate_trace(stderr, "DROP_SHORT", ms_since(t_trace0, t_now), voice_ms, 0);
                            std::fprintf(stderr, "[VG] DROP_SHORT_DETAIL silent=%lldms min_voice=%dms\n",
                                (long long) silent_ms,
                                params.min_voice_ms);
                            std::fflush(stderr);
                        }
                        audio.clear();
                        in_voice = false;
                        trace_silence_started = false;
                        t_last = t_now;
                        continue;
                    }

                    // Reset for next utterance.
                    audio.clear();
                    in_voice = false;
                    trace_silence_started = false;
                } else {
                    t_last = t_now;
                    continue;
                }
            } else {
                t_last = t_now;
                continue;
            }
        }

        if (!have_pcm_block) {
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

            dbg_block_ms = params.length_ms;
        }

        // Activity fraction is cheap and used for conservative near-silence suppression.
        // Compute it consistently across modes so suppression decisions aren't based on a hardcoded 0.
        const bool want_block_stats = params.debug_thankyou || suspect_debug_enabled || params.fast || gated_block;
        const float block_frac = audio_activity_fraction(pcm_block, /*abs_thold=*/0.01f);
        const float block_rms  = want_block_stats ? audio_rms(pcm_block) : 0.0f;
        const float vad_frac   = want_block_stats ? audio_activity_fraction(pcm_vad_window, /*abs_thold=*/0.01f) : 0.0f;
        const float vad_rms    = want_block_stats ? audio_rms(pcm_vad_window) : 0.0f;

        // Guard: some near-silence / tiny-noise blocks can slip through voice-gate and cause
        // hallucinations like "you" / "Thank you" even when Whisper reports no_speech_prob ~ 0.
        // Drop only when the block is truly very quiet.
        const bool near_silence_block = (block_rms < 0.005f && block_frac < 0.02f);
        if (near_silence_block && (params.fast || gated_block)) {
            if (suspect_debug_enabled) {
                const int64_t t_ms = ms_since(t_trace0, t_now);
                const std::string wav = maybe_dump_suspect_wav(params, t_ms, iter, "dropped-near-silence", pcm_block);
                maybe_log_suspect_event_jsonl(params,
                    t_ms,
                    iter,
                    gated_block,
                    dbg_block_ms,
                    dbg_voice_ms,
                    dbg_silent_ms,
                    dbg_trim_ms,
                    pcm_block.size(),
                    block_frac,
                    block_rms,
                    /*effective_language*/params.language,
                    /*max_no_speech_prob*/-1.0f,
                    /*n_segments*/0,
                    "(dropped near silence before whisper)",
                    /*suppressed*/true,
                    "near_silence_block",
                    wav);
            }
            t_last = t_now;
            continue;
        }

        // Fast-mode guard: keyboard clicks / near-silence can trigger VAD and cause hallucinations like "thank you".
        // If the block has very low activity, drop it and clear the buffer so we don't retrigger on the same click.
        if (params.fast && !gated_block) {
            if (block_frac < 0.01f) {
                if (suspect_debug_enabled) {
                    const int64_t t_ms = ms_since(t_trace0, t_now);
                    const std::string wav = maybe_dump_suspect_wav(params, t_ms, iter, "dropped-low-activity", pcm_block);
                    maybe_log_suspect_event_jsonl(params,
                        t_ms,
                        iter,
                        /*gated_block*/false,
                        dbg_block_ms,
                        /*voice_ms*/-1,
                        /*silent_ms*/-1,
                        /*trim_ms*/-1,
                        pcm_block.size(),
                        block_frac,
                        block_rms,
                        /*effective_language*/params.language,
                        /*max_no_speech_prob*/-1.0f,
                        /*n_segments*/0,
                        "(dropped low activity before whisper)",
                        /*suppressed*/true,
                        "fast_block_frac<0.01",
                        wav);
                }
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
        std::vector<float> dbg_seg_ns;
        std::vector<int> dbg_seg_tok;
        std::vector<int64_t> dbg_seg_t0;
        std::vector<int64_t> dbg_seg_t1;
        if (params.debug_thankyou) {
            dbg_seg_ns.reserve(n_segments);
            dbg_seg_tok.reserve(n_segments);
            dbg_seg_t0.reserve(n_segments);
            dbg_seg_t1.reserve(n_segments);
        }
        for (int i = 0; i < n_segments; ++i) {
            const float ns = whisper_full_get_segment_no_speech_prob(ctx, i);
            max_no_speech_prob = std::max(max_no_speech_prob, ns);
            if (params.debug_thankyou) {
                dbg_seg_ns.push_back(ns);
                dbg_seg_tok.push_back(whisper_full_n_tokens(ctx, i));
                dbg_seg_t0.push_back(whisper_full_get_segment_t0(ctx, i));
                dbg_seg_t1.push_back(whisper_full_get_segment_t1(ctx, i));
            }
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

        const bool is_thanks = is_exact_thank_you(text);
        const bool is_you = is_exact_you(text);
        const bool is_garbage = is_short_garbage_like(text);

        // User-requested hard suppression: only for the exact single-word output "you".
        // This does not affect longer sentences containing the word "you".
        const bool suppress_lone_you = params.suppress_lone_you && is_you;

        // Extra audio-based guards for cases where no_speech_prob is unreliable (observed as 0.0 on near silence).
        const bool looks_like_silence_audio = (block_rms < 0.006f && block_frac < 0.03f);
        // Slightly looser: catches low-energy noise bursts that can decode as common short phrases.
        const bool looks_like_low_energy_noise = (block_rms < 0.020f && block_frac < 0.15f);

        const bool suppress_thanks = params.fast && is_thanks &&
            (max_no_speech_prob >= 0.80f || looks_like_silence_audio || looks_like_low_energy_noise);

        // Suppress common near-silence end-of-utterance garbage.
        // Keep this conservative: only when Whisper itself says it's probably no-speech.
        // If the confidence is extremely high, allow suppression even with some background noise.
        const bool suppress_silence_garbage =
            (is_you || is_garbage) &&
            (
                looks_like_silence_audio ||
                looks_like_low_energy_noise ||
                (max_no_speech_prob >= 0.95f) ||
                (max_no_speech_prob >= 0.85f && block_frac < 0.02f)
            );

        if (suspect_debug_enabled && (is_thanks || is_you || is_garbage)) {
            const int64_t t_ms = ms_since(t_trace0, t_now);
            const bool suppressed = suppress_thanks || suppress_silence_garbage || suppress_lone_you;
            const char * reason = suppressed
                ? (suppress_thanks ? "suppress_thanks" : (suppress_lone_you ? "suppress_lone_you" : "suppress_silence_garbage"))
                : "suspect_output";
            const std::string wav = maybe_dump_suspect_wav(params, t_ms, iter, text, pcm_block);
            maybe_log_suspect_event_jsonl(params,
                t_ms,
                iter,
                gated_block,
                dbg_block_ms,
                dbg_voice_ms,
                dbg_silent_ms,
                dbg_trim_ms,
                pcm_block.size(),
                block_frac,
                block_rms,
                effective_language,
                max_no_speech_prob,
                n_segments,
                text,
                suppressed,
                reason,
                wav);
        }

        if (params.debug_thankyou && is_thanks) {
            std::fprintf(stderr,
                "[DBG thankyou] suppress=%d max_no_speech=%.2f block: frac=%.3f rms=%.6f vad: frac=%.3f rms=%.6f segs=%d\n",
                suppress_thanks ? 1 : 0,
                max_no_speech_prob,
                block_frac,
                block_rms,
                vad_frac,
                vad_rms,
                n_segments);
            for (int i = 0; i < n_segments; ++i) {
                // whisper segment times are in 10ms units
                const long long t0_ms = (long long) (dbg_seg_t0[i] * 10);
                const long long t1_ms = (long long) (dbg_seg_t1[i] * 10);
                std::fprintf(stderr,
                    "  [DBG thankyou] seg=%d ns=%.2f tok=%d t=%lld-%lld(ms)\n",
                    i,
                    dbg_seg_ns[i],
                    dbg_seg_tok[i],
                    t0_ms,
                    t1_ms);
            }
        }

        // Tiny models can hallucinate short polite phrases after an utterance or during near-silence.
        // Only suppress this in fast mode AND only when whisper itself says it's likely no-speech.
        if (suppress_thanks) {
            t_last = t_now;
            continue;
        }

        if (suppress_lone_you) {
            t_last = t_now;
            continue;
        }

        if (suppress_silence_garbage) {
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

        const size_t k_wrap_cols = 30;
        const std::string text_wrapped = (text.size() > k_wrap_cols) ? wrap_text_wordwise_cols(text, k_wrap_cols) : text;

        std::printf("[%d] %s\n", iter++, text_wrapped.c_str());
        std::fflush(stdout);

        // Enqueue for Streamer.bot sending (length-based throttling handled by worker thread).
        bot_sender.enqueue(streamerbot_send_item{ text_wrapped, text.size() });

        last_sent = text;

        t_last = t_now;
    }

    bot_sender.stop_and_join(/*drain*/true);

    audio.pause();
    if (vctx) whisper_vad_free(vctx);
    if (!params.quiet) {
        whisper_print_timings(ctx);
    }
    whisper_free(ctx);
    return 0;
}

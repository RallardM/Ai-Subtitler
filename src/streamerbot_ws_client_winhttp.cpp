#include "streamerbot_ws_client.h"

#ifdef _WIN32

#include <windows.h>

#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "json.hpp"

using nlohmann::json;

static std::string win32_last_error_string(DWORD err_code) {
    if (err_code == 0) {
        return {};
    }

    auto format_msg = [&](DWORD flags, HMODULE module) -> std::string {
        LPSTR buf = nullptr;
        const DWORD len = FormatMessageA(flags, module, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR) &buf, 0, nullptr);
        std::string msg;
        if (len && buf) {
            msg.assign(buf, len);
            while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) {
                msg.pop_back();
            }
        }
        if (buf) {
            LocalFree(buf);
        }
        return msg;
    };

    std::string msg;
    {
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        msg = format_msg(flags, nullptr);
    }

    // Many WinHTTP errors (e.g. 12030 / 0x2EFE) live in winhttp.dll message table.
    if (msg.empty()) {
        HMODULE h_winhttp = GetModuleHandleW(L"winhttp.dll");
        bool should_free = false;
        if (!h_winhttp) {
            h_winhttp = LoadLibraryW(L"winhttp.dll");
            should_free = (h_winhttp != nullptr);
        }
        if (h_winhttp) {
            const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
            msg = format_msg(flags, h_winhttp);
        }
        if (should_free && h_winhttp) {
            FreeLibrary(h_winhttp);
        }
    }

    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), " (0x%08lx)", (unsigned long) err_code);
    msg += tmp;

    return msg;
}

streamerbot_ws_client::streamerbot_ws_client() = default;

streamerbot_ws_client::~streamerbot_ws_client() {
    close();
}

bool streamerbot_ws_client::is_connected() const {
    return m_h_websocket != nullptr;
}

void streamerbot_ws_client::close() {
    if (m_h_websocket) {
        WinHttpWebSocketClose((HINTERNET) m_h_websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle((HINTERNET) m_h_websocket);
        m_h_websocket = nullptr;
    }
    if (m_h_request) {
        WinHttpCloseHandle((HINTERNET) m_h_request);
        m_h_request = nullptr;
    }
    if (m_h_connect) {
        WinHttpCloseHandle((HINTERNET) m_h_connect);
        m_h_connect = nullptr;
    }
    if (m_h_session) {
        WinHttpCloseHandle((HINTERNET) m_h_session);
        m_h_session = nullptr;
    }
}

std::wstring streamerbot_ws_client::to_wstring_utf8(const std::string & s) {
    if (s.empty()) {
        return {};
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring ws(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), ws.data(), wlen);
    return ws;
}

std::string streamerbot_ws_client::to_string_utf8(const std::wstring & s) {
    if (s.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static bool starts_with(const std::string & s, const char * prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

bool streamerbot_ws_client::parse_ws_url(const std::string & url, ws_url_parts & parts, std::string & err) {
    err.clear();
    parts = {};

    std::string u = url;
    while (!u.empty() && std::isspace((unsigned char) u.back())) u.pop_back();
    size_t i = 0;
    while (i < u.size() && std::isspace((unsigned char) u[i])) i++;
    u = u.substr(i);

    if (starts_with(u, "ws://")) {
        parts.secure = false;
        u = u.substr(5);
        parts.port = 80;
    } else if (starts_with(u, "wss://")) {
        parts.secure = true;
        u = u.substr(6);
        parts.port = 443;
    } else {
        err = "url must start with ws:// or wss://";
        return false;
    }

    std::string hostport;
    std::string path = "/";
    const size_t slash = u.find('/');
    if (slash == std::string::npos) {
        hostport = u;
    } else {
        hostport = u.substr(0, slash);
        path = u.substr(slash);
        if (path.empty()) {
            path = "/";
        }
    }

    if (hostport.empty()) {
        err = "missing host";
        return false;
    }

    std::string host = hostport;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        host = hostport.substr(0, colon);
        const std::string port_str = hostport.substr(colon + 1);
        if (port_str.empty()) {
            err = "missing port after ':'";
            return false;
        }
        const int port = std::stoi(port_str);
        if (port <= 0 || port > 65535) {
            err = "invalid port";
            return false;
        }
        parts.port = (unsigned short) port;
    }

    parts.host = to_wstring_utf8(host);
    parts.path = to_wstring_utf8(path);

    return true;
}

bool streamerbot_ws_client::sha256_base64(const std::string & data, std::string & out_b64, std::string & err) {
    err.clear();
    out_b64.clear();

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD obj_len = 0;
    DWORD hash_len = 0;
    DWORD cb = 0;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st != 0) {
        err = "BCryptOpenAlgorithmProvider failed";
        return false;
    }

    st = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR) &obj_len, sizeof(obj_len), &cb, 0);
    if (st != 0) {
        err = "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    st = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR) &hash_len, sizeof(hash_len), &cb, 0);
    if (st != 0) {
        err = "BCryptGetProperty(BCRYPT_HASH_LENGTH) failed";
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    std::string hash_obj(obj_len, 0);
    std::string digest(hash_len, 0);

    st = BCryptCreateHash(alg, &hash, (PUCHAR) hash_obj.data(), obj_len, nullptr, 0, 0);
    if (st != 0) {
        err = "BCryptCreateHash failed";
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    st = BCryptHashData(hash, (PUCHAR) data.data(), (ULONG) data.size(), 0);
    if (st != 0) {
        err = "BCryptHashData failed";
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    st = BCryptFinishHash(hash, (PUCHAR) digest.data(), hash_len, 0);
    if (st != 0) {
        err = "BCryptFinishHash failed";
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    DWORD b64_len = 0;
    if (!CryptBinaryToStringA((const BYTE *) digest.data(), (DWORD) digest.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64_len)) {
        err = "CryptBinaryToStringA size failed: " + win32_last_error_string(GetLastError());
        return false;
    }

    std::string b64(b64_len, 0);
    if (!CryptBinaryToStringA((const BYTE *) digest.data(), (DWORD) digest.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &b64_len)) {
        err = "CryptBinaryToStringA failed: " + win32_last_error_string(GetLastError());
        return false;
    }

    if (!b64.empty() && b64.back() == '\0') {
        b64.pop_back();
    }

    out_b64 = b64;
    return true;
}

bool streamerbot_ws_client::build_authentication(const std::string & password, const std::string & salt_b64, const std::string & challenge_b64, std::string & out_auth, std::string & err) {
    std::string secret;
    if (!sha256_base64(password + salt_b64, secret, err)) {
        return false;
    }
    if (!sha256_base64(secret + challenge_b64, out_auth, err)) {
        return false;
    }
    return true;
}

bool streamerbot_ws_client::connect_internal(const ws_url_parts & parts, std::string & err) {
    err.clear();
    close();

    m_h_session = WinHttpOpen(L"ai-subtitler-streamerbot/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_h_session) {
        err = "WinHttpOpen failed: " + win32_last_error_string(GetLastError());
        return false;
    }

    m_h_connect = WinHttpConnect((HINTERNET) m_h_session, parts.host.c_str(), parts.port, 0);
    if (!m_h_connect) {
        err = "WinHttpConnect failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    m_h_request = WinHttpOpenRequest((HINTERNET) m_h_connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!m_h_request) {
        err = "WinHttpOpenRequest failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    // Per WinHTTP docs, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET takes no data.
    if (!WinHttpSetOption((HINTERNET) m_h_request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        err = "WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    if (!WinHttpSendRequest((HINTERNET) m_h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = "WinHttpSendRequest failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    if (!WinHttpReceiveResponse((HINTERNET) m_h_request, nullptr)) {
        err = "WinHttpReceiveResponse failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    m_h_websocket = WinHttpWebSocketCompleteUpgrade((HINTERNET) m_h_request, 0);
    if (!m_h_websocket) {
        err = "WinHttpWebSocketCompleteUpgrade failed: " + win32_last_error_string(GetLastError());
        close();
        return false;
    }

    WinHttpCloseHandle((HINTERNET) m_h_request);
    m_h_request = nullptr;

    return true;
}

bool streamerbot_ws_client::recv_text_message(std::string & msg, std::string & err) {
    err.clear();
    msg.clear();

    if (!m_h_websocket) {
        err = "not connected";
        return false;
    }

    std::string buf;
    buf.resize(16 * 1024);

    WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    DWORD bytes_read = 0;
    std::string out;

    while (true) {
        bytes_read = 0;
        buffer_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD rc = WinHttpWebSocketReceive((HINTERNET) m_h_websocket, buf.data(), (DWORD) buf.size(), &bytes_read, &buffer_type);
        if (rc != NO_ERROR) {
            err = "WinHttpWebSocketReceive failed: " + win32_last_error_string(rc);
            return false;
        }

        if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            err = "websocket closed by server";
            return false;
        }

        out.append(buf.data(), buf.data() + bytes_read);

        if (buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            // message complete
            break;
        }

        // if *_FRAGMENT_BUFFER_TYPE, loop until complete
    }

    msg = out;
    return true;
}

bool streamerbot_ws_client::send_text_message(const std::string & msg, std::string & err) {
    err.clear();
    if (!m_h_websocket) {
        err = "not connected";
        return false;
    }
    const DWORD rc = WinHttpWebSocketSend((HINTERNET) m_h_websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID) msg.data(), (DWORD) msg.size());
    if (rc != NO_ERROR) {
        err = "WinHttpWebSocketSend failed: " + win32_last_error_string(rc);
        return false;
    }
    return true;
}

bool streamerbot_ws_client::connect_and_handshake(const streamerbot_ws_config & cfg, std::string & err) {
    ws_url_parts parts;
    if (!parse_ws_url(cfg.url, parts, err)) {
        return false;
    }

    if (!connect_internal(parts, err)) {
        return false;
    }

    // Expect Hello
    std::string hello;
    if (!recv_text_message(hello, err)) {
        close();
        return false;
    }

    json j;
    try {
        j = json::parse(hello);
    } catch (const std::exception & e) {
        err = std::string("failed to parse Hello JSON: ") + e.what();
        close();
        return false;
    }

    if (!j.contains("request") || j["request"].get<std::string>() != "Hello") {
        // Some servers might send other messages first; keep going.
        return true;
    }

    if (!j.contains("authentication")) {
        return true;
    }

    if (!cfg.password.has_value()) {
        // Authentication is enabled but may not be enforced for DoAction; allow continuing.
        return true;
    }

    const auto & a = j["authentication"];
    if (!a.contains("salt") || !a.contains("challenge")) {
        return true;
    }

    const std::string salt = a["salt"].get<std::string>();
    const std::string challenge = a["challenge"].get<std::string>();

    std::string auth;
    if (!build_authentication(*cfg.password, salt, challenge, auth, err)) {
        close();
        return false;
    }

    json auth_req;
    auth_req["request"] = "Authenticate";
    auth_req["id"] = "ai-subtitler-auth";
    auth_req["authentication"] = auth;

    if (!send_text_message(auth_req.dump(), err)) {
        close();
        return false;
    }

    // Best-effort: read one response, but don't fail the connection if it doesn't arrive immediately.
    // Some setups may not enforce auth for DoAction.
    return true;
}

bool streamerbot_ws_client::do_action_text(const streamerbot_ws_config & cfg, const std::string & text, std::string & err) {
    err.clear();
    if (!is_connected()) {
        err = "not connected";
        return false;
    }

    json req;
    req["request"] = "DoAction";
    req["id"] = "ai-subtitler-doaction";
    req["action"] = json::object();
    req["action"]["name"] = cfg.action_name;
    req["args"] = json::object();
    req["args"][cfg.arg_key] = text;

    return send_text_message(req.dump(), err);
}

#else

streamerbot_ws_client::streamerbot_ws_client() = default;
streamerbot_ws_client::~streamerbot_ws_client() = default;
bool streamerbot_ws_client::connect_and_handshake(const streamerbot_ws_config &, std::string & err) { err = "WinHTTP WebSocket client is only implemented on Windows"; return false; }
void streamerbot_ws_client::close() {}
bool streamerbot_ws_client::is_connected() const { return false; }
bool streamerbot_ws_client::do_action_text(const streamerbot_ws_config &, const std::string &, std::string & err) { err = "not supported"; return false; }

#endif

#pragma once

#include <optional>
#include <string>

struct streamerbot_ws_config {
    std::string url = "ws://127.0.0.1:8080/";
    std::string action_name = "AI Subtitler";
    std::string arg_key = "AiText";
    std::optional<std::string> password;
};

class streamerbot_ws_client {
public:
    streamerbot_ws_client();
    ~streamerbot_ws_client();

    streamerbot_ws_client(const streamerbot_ws_client &) = delete;
    streamerbot_ws_client & operator=(const streamerbot_ws_client &) = delete;

    bool connect_and_handshake(const streamerbot_ws_config & cfg, std::string & err);
    void close();

    bool is_connected() const;
    bool do_action_text(const streamerbot_ws_config & cfg, const std::string & text, std::string & err);

private:
    struct ws_url_parts {
        bool secure = false;
        std::wstring host;
        unsigned short port = 0;
        std::wstring path;
    };

    static bool parse_ws_url(const std::string & url, ws_url_parts & parts, std::string & err);

    static std::wstring to_wstring_utf8(const std::string & s);
    static std::string to_string_utf8(const std::wstring & s);

    bool connect_internal(const ws_url_parts & parts, std::string & err);
    bool recv_text_message(std::string & msg, std::string & err);
    bool send_text_message(const std::string & msg, std::string & err);

    static bool sha256_base64(const std::string & data, std::string & out_b64, std::string & err);
    static bool build_authentication(const std::string & password, const std::string & salt_b64, const std::string & challenge_b64, std::string & out_auth, std::string & err);

private:
    void * m_h_session = nullptr;
    void * m_h_connect = nullptr;
    void * m_h_request = nullptr;
    void * m_h_websocket = nullptr;
};

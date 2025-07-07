#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <WinSock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include "tlsclient.cpp"
#include "json_minimal.h"
#include "chunked_decode.h"

#pragma comment(lib, "ws2_32.lib")

// Helper: Read all HTTP response (blocking)
std::string tls_http_read_all(tls_client& client) {
    std::string response;
    char buf[4096];
    int n;

    // Step 1: Read until end of headers
    while (response.find("\r\n\r\n") == std::string::npos) {
        n = client.recv(buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, n);
    }

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) return response; // malformed

    size_t body_start = header_end + 4;
    std::string headers = response.substr(0, header_end);

    // Check for chunked transfer-encoding (case-insensitive)
    bool is_chunked = false;
    std::string chunked_key = "transfer-encoding: chunked";
    for (size_t pos = 0; pos + chunked_key.size() <= headers.size(); ++pos) {
        if (std::equal(chunked_key.begin(), chunked_key.end(), headers.begin() + pos,
            [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            is_chunked = true;
            break;
        }
    }

    // Handle Content-Length (as before)
    size_t content_length = 0;
    bool has_content_length = false;
    std::string search = "content-length:";
    size_t cl_pos = std::string::npos;
    for (size_t pos = 0; pos + search.size() <= headers.size(); ++pos) {
        if (std::equal(search.begin(), search.end(), headers.begin() + pos,
            [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            cl_pos = pos + search.size();
            has_content_length = true;
            break;
        }
    }
    if (has_content_length) {
        while (cl_pos < headers.size() && isspace(headers[cl_pos])) ++cl_pos;
        size_t cl_end = headers.find("\r\n", cl_pos);
        if (cl_end != std::string::npos) {
            std::string lenstr = headers.substr(cl_pos, cl_end - cl_pos);
            content_length = std::stoul(lenstr);
        }
    }

    // Step 2: Read the body
    if (has_content_length) {
        while ((response.size() - body_start) < content_length) {
            n = client.recv(buf, std::min<int>(sizeof(buf), (int)(content_length - (response.size() - body_start))));
            if (n <= 0) break;
            response.append(buf, n);
        }
        if ((response.size() - body_start) > content_length)
            response.resize(body_start + content_length);
    }
    else {
        // No Content-Length: read until connection close
        while ((n = client.recv(buf, sizeof(buf))) > 0)
            response.append(buf, n);
    }

    // Step 3: If chunked, decode body
    if (is_chunked) {
        std::string decoded = decode_chunked_body(response.substr(body_start));
        return response.substr(0, body_start) + decoded;
    }
    else {
        return response;
    }
}

// Helper: Get HTTP body (skip headers)
std::string get_http_body(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return resp.substr(pos + 4);
}

// Parse master playlist for qualities
struct Quality { std::string name, url; };
std::vector<Quality> parse_m3u8(const std::string& text) {
    std::vector<Quality> out;
    std::istringstream iss(text);
    std::string line, last;
    while (std::getline(iss, line)) {
        if (line.find("#EXT-X-STREAM-INF:") == 0) { last = line; continue; }
        if (!line.empty() && line[0] != '#') {
            std::string q = "Unknown";
            auto res = last.find("RESOLUTION=");
            if (res != std::string::npos)
                q = last.substr(res + 11, 7);
            out.push_back({ q, line });
            last.clear();
        }
    }
    return out;
}

int main(int argc, char** argv) {
    std::cout << "[DEBUG] Program started\n";
    if (argc != 2) {
        std::cout << "Usage: mytls <twitch_channel>\n";
        return 1;
    }
    std::string channel = argv[1];
    std::cout << "[DEBUG] Channel: " << channel << "\n";

    WSADATA wsad;
    int wsRet = WSAStartup(MAKEWORD(2, 2), &wsad);
    std::cout << "[DEBUG] WSAStartup returned: " << wsRet << "\n";
    if (wsRet != 0) {
        std::cerr << "[ERROR] WSAStartup failed\n";
        return 1;
    }
    tls_client::init_global();
    std::cout << "[DEBUG] tls_client::init_global() called\n";

    // --- Step 1: POST GQL for stream access token (PlaybackAccessToken & AdRequestHandling) ---
    std::string gql_body =
        "["
        "{\"operationName\":\"PlaybackAccessToken\","
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"0828119ded1c13477966434e15800ff57ddacf13ba1911c129dc2200705b0712\"}},"
        "\"variables\":{\"isLive\":true,\"login\":\"" + channel + "\",\"isVod\":false,\"vodID\":\"\",\"playerType\":\"embed\"}"
        "},"
        "{\"operationName\":\"AdRequestHandling\","
        "\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"61a5ecca6da3d924efa9dbde811e051b8a10cb6bd0fe22c372c2f4401f3e88d1\"}},"
        "\"variables\":{\"isLive\":true,\"login\":\"" + channel + "\",\"isVOD\":false,\"vodID\":\"\",\"isCollection\":false,\"collectionID\":\"\"}"
        "}"
        "]";

    std::cout << "[DEBUG] GQL POST body:\n" << gql_body << "\n";
    std::cout << "[DEBUG] gql_body.size(): " << gql_body.size() << "\n";

    std::ostringstream oss;
    oss << "POST /gql HTTP/1.1\r\n"
        << "Host: gql.twitch.tv\r\n"
        << "Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n"
        << "User-Agent: Mozilla/5.0\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << gql_body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << gql_body;

    std::string req = oss.str();
    std::ofstream raw_http_log_gql("http_request_gql.log", std::ios::binary);
    raw_http_log_gql.write(req.data(), req.size());
    raw_http_log_gql.close();
    std::cout << "[DEBUG] Full GQL HTTP request:\n" << req << "\n";
    std::cout << "[DEBUG] req.size(): " << req.size() << "\n";

    tls_client client;
    std::cout << "[DEBUG] Opening TLS connection to gql.twitch.tv:443\n";
    if (client.open("gql.twitch.tv", 443)) {
        std::cerr << "[ERROR] TLS connect failed: " << client.errmsg() << "\n";
        return 1;
    }
    std::cout << "[DEBUG] TLS connection established\n";

    std::cout << "[DEBUG] Sending GQL request...\n";
    int sendret = client.send((char*)req.data(), (int)req.size());
    std::cout << "[DEBUG] Sent bytes: " << sendret << "\n";
    if (sendret != (int)req.size()) {
        std::cerr << "[ERROR] TLS send failed: " << client.errmsg() << "\n";
        return 1;
    }

    std::cout << "[DEBUG] Reading GQL response...\n";
    std::string gqlresp = tls_http_read_all(client);
    // Dump the full plaintext GQL HTTP response
    std::ofstream gql_resp_log("gql_response.log", std::ios::binary);
    gql_resp_log.write(gqlresp.data(), gqlresp.size());
    gql_resp_log.close();
    std::cout << "[DEBUG] GQL response received. Size: " << gqlresp.size() << "\n";
    std::cout << "[DEBUG] First 500 chars of GQL response:\n" << gqlresp.substr(0, 500) << "\n";
    client.close();

    std::string body = get_http_body(gqlresp);

    std::cout << "[DEBUG] GQL HTTP body (first 500 chars):\n" << body.substr(0, 500) << "\n";

    // --- Use json_minimal.h to parse ---
    JsonValue root = parse_json(body);
    if (root.type != JsonValue::Array || root.size() < 1) {
        std::cerr << "[ERROR] GQL parse: not an array or empty\n";
        return 2;
    }
    std::string sig = root[0]["data"]["streamPlaybackAccessToken"]["signature"].as_str();
    std::string token = root[0]["data"]["streamPlaybackAccessToken"]["value"].as_str();
    if (sig.empty() || token.empty()) {
        std::cerr << "[ERROR] GQL parse: missing signature or token\n";
        return 2;
    }
    std::cout << "[DEBUG] Extracted signature: " << sig << "\n";
    std::cout << "[DEBUG] Extracted token (first 80 chars): " << token.substr(0, 80) << (token.size() > 80 ? "..." : "") << "\n";

    // --- Step 2: GET HLS playlist with signature & token as query params ---
    std::ostringstream urloss;
    urloss << "/api/channel/hls/" << channel << ".m3u8"
           << "?allow_source=true"
           << "&allow_audio_only=true"
           << "&fast_bread=true"
           << "&sig=" << sig
           << "&token=" << token;

    std::cout << "[DEBUG] HLS playlist GET path: " << urloss.str() << "\n";

    std::ostringstream oss2;
    oss2 << "GET " << urloss.str()
         << " HTTP/1.1\r\n"
         << "Host: usher.ttvnw.net\r\n"
         << "User-Agent: Mozilla/5.0\r\n"
         << "Connection: close\r\n\r\n";

    std::string req2 = oss2.str();
    std::ofstream raw_http_log_hls("http_request_hls.log", std::ios::binary);
    raw_http_log_hls.write(req2.data(), req2.size());  // Fixed: was writing req instead of req2
    raw_http_log_hls.close();
    std::cout << "[DEBUG] Full HLS HTTP request:\n" << req2 << "\n";

    tls_client usher;
    std::cout << "[DEBUG] Opening TLS connection to usher.ttvnw.net:443\n";
    if (usher.open("usher.ttvnw.net", 443)) {
        std::cerr << "[ERROR] TLS connect failed: " << usher.errmsg() << "\n";
        return 1;
    }
    std::cout << "[DEBUG] TLS connection established\n";

    std::cout << "[DEBUG] Sending HLS playlist request...\n";
    sendret = usher.send((char*)req2.data(), (int)req2.size());
    std::cout << "[DEBUG] Sent bytes: " << sendret << "\n";
    if (sendret != (int)req2.size()) {
        std::cerr << "[ERROR] TLS send failed: " << usher.errmsg() << "\n";
        return 1;
    }

    std::cout << "[DEBUG] Reading HLS playlist response...\n";
    std::string m3u8resp = tls_http_read_all(usher);
    // Dump the full plaintext HLS HTTP response
    std::ofstream hls_resp_log("hls_response.log", std::ios::binary);
    hls_resp_log.write(m3u8resp.data(), m3u8resp.size());
    hls_resp_log.close();
    std::cout << "[DEBUG] Playlist response received. Size: " << m3u8resp.size() << "\n";
    std::cout << "[DEBUG] First 500 chars of playlist response:\n" << m3u8resp.substr(0, 500) << "\n";
    usher.close();

    std::string playlist = get_http_body(m3u8resp);
    std::cout << "[DEBUG] Extracted playlist body. Size: " << playlist.size() << "\n";
    std::cout << "[DEBUG] First 500 chars of playlist body:\n" << playlist.substr(0, 500) << "\n";

    auto qualities = parse_m3u8(playlist);
    std::cout << "[DEBUG] Number of qualities found: " << qualities.size() << "\n";
    if (qualities.empty()) {
        std::cerr << "[ERROR] No qualities found\n";
        return 3;
    }

    std::cout << "Available qualities:\n";
    for (size_t i = 0; i < qualities.size(); ++i)
        std::cout << "[" << i << "] " << qualities[i].name << "\n";
    std::cout << "Select: ";
    int sel = 0;
    std::cin >> sel;
    if (sel < 0 || sel >= (int)qualities.size()) {
        std::cerr << "[ERROR] Invalid quality selection\n";
        return 4;
    }
    std::cout << "[DEBUG] User selected quality index: " << sel << "\n";
    std::cout << "Playlist URL: " << qualities[sel].url << "\n";
    WSACleanup();
    std::cout << "[DEBUG] Program finished\n";
    return 0;
}
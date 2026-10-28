// Minimal blocking HTTP/1.1 server and client over POSIX sockets.
// Enough for JSON-over-HTTP between our own services and for calling Ollama.
#pragma once
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace bg {

struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers;  // header names lower-cased
};
struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};
struct HttpReply {
    int status = 0;
    std::string body;
};

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline void set_timeouts(int fd, double sec) {
    if (sec < 0.01) sec = 0.01;  // a zero timeval would mean "wait forever"
    timeval tv{};
    tv.tv_sec = static_cast<long>(sec);
    tv.tv_usec = static_cast<long>((sec - static_cast<double>(tv.tv_sec)) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
inline bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}
inline const char* reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

// ------------------------------------------------------------------ server
inline void handle_conn(int fd, const std::function<Response(const Request&)>& handler) {
    set_timeouts(fd, 30);
    std::string buf;
    char tmp[4096];
    size_t hend;
    while ((hend = buf.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0 || buf.size() > 65536) { ::close(fd); return; }
        buf.append(tmp, static_cast<size_t>(n));
    }
    Request rq;
    std::istringstream head(buf.substr(0, hend));
    std::string line, version;
    std::getline(head, line);
    std::istringstream first(line);
    first >> rq.method >> rq.path >> version;
    size_t q = rq.path.find('?');
    if (q != std::string::npos) rq.path.resize(q);
    while (std::getline(head, line)) {
        size_t c = line.find(':');
        if (c != std::string::npos) rq.headers[to_lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }
    size_t clen = 0;
    auto it = rq.headers.find("content-length");
    if (it != rq.headers.end()) {
        try { clen = std::stoul(it->second); } catch (...) { clen = 0; }
    }
    Response rs;
    if (clen > 1000000) {
        rs.status = 413;
        rs.body = "{\"error\":\"too large\"}";
    } else {
        std::string body = buf.substr(hend + 4);
        while (body.size() < clen) {
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            body.append(tmp, static_cast<size_t>(n));
        }
        rq.body = body.substr(0, std::min(clen, body.size()));
        try { rs = handler(rq); }
        catch (...) { rs.status = 500; rs.body = "{\"error\":\"internal\"}"; }
    }
    std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " " + reason_phrase(rs.status) +
                      "\r\nContent-Type: " + rs.content_type +
                      "\r\nContent-Length: " + std::to_string(rs.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + rs.body;
    send_all(fd, out);
    ::close(fd);
}

// Blocks forever. One detached thread per connection.
inline void serve(int port, std::function<Response(const Request&)> handler) {
    signal(SIGPIPE, SIG_IGN);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 128) < 0) {
        std::perror("bind/listen");
        std::exit(1);
    }
    for (;;) {
        int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        std::thread([c, handler]() { handle_conn(c, handler); }).detach();
    }
}

// ------------------------------------------------------------------ client
inline int connect_to(const std::string& host, int port, double timeout_sec) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        set_timeouts(fd, timeout_sec);
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

inline bool tcp_reachable(const std::string& host, int port, int timeout_sec = 2) {
    int fd = connect_to(host, port, timeout_sec);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

inline std::string dechunk(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find("\r\n", i);
        if (e == std::string::npos) break;
        size_t len = 0;
        try { len = std::stoul(s.substr(i, e - i), nullptr, 16); } catch (...) { break; }
        if (len == 0) break;
        i = e + 2;
        if (i + len > s.size()) { out.append(s, i, std::string::npos); break; }
        out.append(s, i, len);
        i += len + 2;
    }
    return out;
}

// timeout_sec is a TOTAL deadline for connect + send + receive, not a per-read timeout.
inline std::optional<HttpReply> http_post(const std::string& host, int port, const std::string& path,
                                          const std::string& body, double timeout_sec) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(timeout_sec));
    int fd = connect_to(host, port, timeout_sec);
    if (fd < 0) return std::nullopt;
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body;
    if (!send_all(fd, req)) { ::close(fd); return std::nullopt; }
    std::string resp;
    char tmp[8192];
    for (;;) {
        const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { ::close(fd); return std::nullopt; }  // total deadline passed
        set_timeouts(fd, left);
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n == 0) break;                       // server closed: full response received
        if (n < 0) { ::close(fd); return std::nullopt; }  // timeout or error
        resp.append(tmp, static_cast<size_t>(n));
    }
    ::close(fd);
    size_t hend = resp.find("\r\n\r\n");
    if (hend == std::string::npos) return std::nullopt;
    HttpReply r;
    std::istringstream head(resp.substr(0, hend));
    std::string line, ver;
    std::getline(head, line);
    std::istringstream first(line);
    first >> ver >> r.status;
    bool chunked = false;
    while (std::getline(head, line)) {
        std::string l = to_lower(line);
        if (l.rfind("transfer-encoding:", 0) == 0 && l.find("chunked") != std::string::npos) chunked = true;
    }
    std::string payload = resp.substr(hend + 4);
    r.body = chunked ? dechunk(payload) : payload;
    return r;
}

}  // namespace bg

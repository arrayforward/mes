// ============================================================================
// mse/http_server.cpp —— 极简 HTTP/1.1 服务器与客户端实现(原生 socket)
//
// Windows(winsock2)/ Linux(posix)双分支;单线程 accept 循环 + 每连接一个
// detach 处理线程。仅实现本系统需要的子集:Content-Length 请求体、
// Connection: close、JSON 响应。
// ============================================================================

#include "mse/http_server.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mse {
namespace {

// ---- 平台抽象 ----
#ifdef _WIN32
using socket_t = SOCKET;
using socklen_like = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_t s) { ::closesocket(s); }
void shutdown_socket(socket_t s) { ::shutdown(s, SD_BOTH); }

// WSAStartup/WSACleanup 计数管理(服务器与客户端可能并存)
std::mutex g_wsa_mu;
int g_wsa_count = 0;
bool socket_env_acquire() {
    std::lock_guard<std::mutex> lk(g_wsa_mu);
    if (g_wsa_count == 0) {
        WSADATA wd;
        if (::WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    }
    ++g_wsa_count;
    return true;
}
void socket_env_release() {
    std::lock_guard<std::mutex> lk(g_wsa_mu);
    if (g_wsa_count > 0 && --g_wsa_count == 0) ::WSACleanup();
}
#else
using socket_t = int;
using socklen_like = socklen_t;
constexpr socket_t kInvalidSocket = -1;
void close_socket(socket_t s) { ::close(s); }
void shutdown_socket(socket_t s) { ::shutdown(s, SHUT_RDWR); }
bool socket_env_acquire() { return true; }
void socket_env_release() {}
#endif

bool send_all(socket_t s, const char* data, size_t len) {
    while (len > 0) {
        const int n = ::send(s, data, static_cast<int>(len), 0);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string to_lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// 百分号解码:%XX 还原为字节(UTF-8 原样保留),+ → 空格
std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == '+') {
            out += ' ';
        } else if (ch == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2]));
            i += 2;
        } else {
            out += ch;
        }
    }
    return out;
}

// ---- 连接处理:读请求行+头部(到 \r\n\r\n)+ Content-Length 字节 body ----
void handle_connection(socket_t conn, const HttpServer::Handler& handler) {
    char buf[8192];
    std::string raw;
    size_t header_end = std::string::npos;
    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
        const int n = ::recv(conn, buf, static_cast<int>(sizeof(buf)), 0);
        if (n <= 0) { close_socket(conn); return; }
        raw.append(buf, static_cast<size_t>(n));
        if (raw.size() > 65536) { close_socket(conn); return; }  // 头部过大,放弃
    }

    // 请求行:METHOD SP target SP version
    HttpRequest req;
    const size_t line_end = raw.find("\r\n");
    const std::string request_line = raw.substr(0, line_end);
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 =
        sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        close_socket(conn);
        return;
    }
    req.method = request_line.substr(0, sp1);
    const std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    if (q == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, q);
        req.query = parse_query(target.substr(q + 1));
    }

    // 头部:只关心 Content-Length
    size_t content_length = 0;
    size_t pos = line_end + 2;
    while (pos < header_end) {
        const size_t eol = raw.find("\r\n", pos);
        const std::string line = raw.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos &&
            to_lower(trim(line.substr(0, colon))) == "content-length") {
            content_length = static_cast<size_t>(
                std::strtoull(trim(line.substr(colon + 1)).c_str(), nullptr, 10));
        }
        pos = eol + 2;
    }

    // body:补足 Content-Length 字节
    const size_t body_off = header_end + 4;
    while (raw.size() - body_off < content_length) {
        const int n = ::recv(conn, buf, static_cast<int>(sizeof(buf)), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    req.body = raw.substr(body_off, content_length);

    HttpResponse resp;
    try {
        resp = handler(req);
    } catch (const std::exception& ex) {
        resp.status = 500;
        resp.body = std::string("{\"error\":\"") + ex.what() + "\"}";
    } catch (...) {
        resp.status = 500;
        resp.body = "{\"error\":\"internal error\"}";
    }

    const std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                            reason_phrase(resp.status) +
                            "\r\nContent-Type: " + resp.content_type +
                            "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                            "\r\nConnection: close\r\n\r\n" + resp.body;
    send_all(conn, out.data(), out.size());
    close_socket(conn);
}

} // namespace

// ---- HttpServer(pimpl) ----

struct HttpServer::Impl {
    socket_t listen_fd = kInvalidSocket;
    sockaddr_in bind_addr{};              // 停机时自连接唤醒 accept 用
    std::atomic<bool> running{false};
    std::thread accept_thread;
    Handler handler;
    uint16_t bound_port = 0;
    bool env_held = false;                // 是否持有 WSA 引用计数
};

HttpServer::HttpServer() : impl_(new Impl) {}

HttpServer::~HttpServer() {
    stop();
    delete impl_;
}

bool HttpServer::listen_on(const std::string& host, uint16_t port, Handler handler) {
    if (impl_->running) return false;
    if (!socket_env_acquire()) return false;

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
        socket_env_release();
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket(fd);
        socket_env_release();
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        close_socket(fd);
        socket_env_release();
        return false;
    }

    // port=0 时取系统实际分配端口
    sockaddr_in bound{};
    socklen_like blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        impl_->bound_port = ntohs(bound.sin_port);

    impl_->listen_fd = fd;
    impl_->bind_addr = addr;
    if (impl_->bound_port != 0) impl_->bind_addr.sin_port = htons(impl_->bound_port);
    impl_->handler = std::move(handler);
    impl_->env_held = true;
    impl_->running = true;
    impl_->accept_thread = std::thread([this] {
        while (impl_->running) {
            sockaddr_in peer{};
            socklen_like plen = sizeof(peer);
            const socket_t conn =
                ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (conn == kInvalidSocket) {
                if (!impl_->running) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!impl_->running) {
                close_socket(conn);
                break;
            }
            std::thread(handle_connection, conn, std::cref(impl_->handler)).detach();
        }
    });
    return true;
}

void HttpServer::stop() {
    impl_->running = false;
    if (impl_->listen_fd != kInvalidSocket) {
        // 自连接唤醒阻塞中的 accept(尤其 Linux:关 fd 不一定唤醒 accept)
        const socket_t wake = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (wake != kInvalidSocket) {
            ::connect(wake, reinterpret_cast<sockaddr*>(&impl_->bind_addr),
                      sizeof(impl_->bind_addr));
            close_socket(wake);
        }
        shutdown_socket(impl_->listen_fd);
        close_socket(impl_->listen_fd);
        impl_->listen_fd = kInvalidSocket;
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    if (impl_->env_held) {
        socket_env_release();
        impl_->env_held = false;
    }
    impl_->bound_port = 0;
}

uint16_t HttpServer::port() const { return impl_->bound_port; }

// ---- 查询串解析 ----

std::map<std::string, std::string> parse_query(const std::string& query_string) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= query_string.size()) {
        const size_t amp = query_string.find('&', pos);
        const std::string pair =
            query_string.substr(pos, amp == std::string::npos ? amp : amp - pos);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos)
                out[url_decode(pair)] = "";
            else
                out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

// ---- 极简同步客户端 ----

std::pair<int, std::string> http_request(const std::string& host, uint16_t port,
                                         const std::string& method, const std::string& path,
                                         const std::string& body) {
    if (!socket_env_acquire()) return {-1, "socket 环境初始化失败"};
    struct EnvGuard {
        ~EnvGuard() { socket_env_release(); }
    } env_guard;
    (void)env_guard;

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) return {-1, "创建 socket 失败"};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket(fd);
        return {-1, "地址解析失败: " + host};
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        return {-1, "连接失败: " + host + ":" + std::to_string(port)};
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    if (!body.empty()) req += "Content-Type: application/json; charset=utf-8\r\n";
    req += "Content-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
    if (!send_all(fd, req.data(), req.size())) {
        close_socket(fd);
        return {-1, "发送请求失败"};
    }

    std::string raw;
    char buf[8192];
    for (;;) {
        const int n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
        if (n <= 0) break;  // Connection: close,对端关连接即收完
        raw.append(buf, static_cast<size_t>(n));
    }
    close_socket(fd);

    // 状态行:HTTP/1.1 <status> ...
    const size_t sp = raw.find(' ');
    if (sp == std::string::npos) return {-1, "响应形状非法"};
    int status = -1;
    try {
        status = std::stoi(raw.substr(sp + 1));
    } catch (...) {
        return {-1, "响应状态码非法"};
    }
    const size_t header_end = raw.find("\r\n\r\n");
    const std::string resp_body =
        header_end == std::string::npos ? std::string{} : raw.substr(header_end + 4);
    return {status, resp_body};
}

} // namespace mse

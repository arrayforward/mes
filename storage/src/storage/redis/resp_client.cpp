#include "resp_client.h"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace storage {
namespace {

[[noreturn]] void io_error(const std::string& what) {
    throw std::runtime_error("resp client: " + what);
}

#ifdef _WIN32
struct WsaInit {
    WsaInit() {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) io_error("WSAStartup failed");
    }
    ~WsaInit() { WSACleanup(); }
};
#endif

} // namespace

RespClient::RespClient(const std::string& host, uint16_t port) {
#ifdef _WIN32
    static WsaInit wsa;
#endif
    // getaddrinfo 解析（IPv4/主机名均可）
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        io_error("cannot resolve " + host);
#ifdef _WIN32
    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ == ~(uintptr_t)0 || connect(sock_, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        io_error("cannot connect to " + host + ":" + port_str);
    }
#else
    fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0 || connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        io_error("cannot connect to " + host + ":" + port_str);
    }
#endif
    freeaddrinfo(res);
}

RespClient::~RespClient() {
#ifdef _WIN32
    if (sock_ != ~uintptr_t(0)) closesocket((SOCKET)sock_);
#else
    if (fd_ >= 0) close(fd_);
#endif
}

void RespClient::fill_buffer() {
    char buf[8192];
#ifdef _WIN32
    int n = recv((SOCKET)sock_, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);
#endif
    if (n <= 0) io_error("connection closed or recv failed");
    buffer_.append(buf, (size_t)n);
}

char RespClient::read_byte() {
    if (buffer_.empty()) fill_buffer();
    char c = buffer_[0];
    buffer_.erase(0, 1);
    return c;
}

std::string RespClient::read_line() {
    for (;;) {
        auto pos = buffer_.find("\r\n");
        if (pos != std::string::npos) {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);
            return line;
        }
        fill_buffer();
    }
}

std::string RespClient::read_bytes(size_t n) {
    while (buffer_.size() < n) fill_buffer();
    std::string out = buffer_.substr(0, n);
    buffer_.erase(0, n);
    return out;
}

void RespClient::send_command(const std::vector<std::string>& args) {
    std::string wire = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        wire += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    size_t sent = 0;
    while (sent < wire.size()) {
#ifdef _WIN32
        int n = send((SOCKET)sock_, wire.data() + sent, (int)(wire.size() - sent), 0);
#else
        ssize_t n = send(fd_, wire.data() + sent, wire.size() - sent, 0);
#endif
        if (n <= 0) io_error("send failed");
        sent += (size_t)n;
    }
}

RespReply RespClient::parse_reply() {
    char prefix = read_byte();
    switch (prefix) {
        case '+': return RespReply{RespReply::Type::SimpleString, read_line(), 0, {}};
        case '-': return RespReply{RespReply::Type::Error, read_line(), 0, {}};
        case ':': return RespReply{RespReply::Type::Integer, "", std::stoll(read_line()), {}};
        case '$': {
            int64_t len = std::stoll(read_line());
            if (len < 0) return RespReply{};
            std::string data = read_bytes((size_t)len);
            read_bytes(2);  // 尾部 \r\n
            return RespReply{RespReply::Type::BulkString, std::move(data), 0, {}};
        }
        case '*': {
            int64_t n = std::stoll(read_line());
            if (n < 0) return RespReply{};
            RespReply r{RespReply::Type::Array, "", 0, {}};
            r.array.reserve((size_t)n);
            for (int64_t i = 0; i < n; ++i) r.array.push_back(parse_reply());
            return r;
        }
        default:
            io_error(std::string("bad reply prefix: ") + prefix);
    }
}

RespReply RespClient::command(const std::vector<std::string>& args) {
    send_command(args);
    return parse_reply();
}

} // namespace storage

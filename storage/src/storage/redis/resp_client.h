#pragma once

// 极简跨平台 RESP2 客户端（Windows Winsock2 / Linux POSIX sockets）。
// 仅实现统一存储层需要的命令子集：一行命令进，一个解析好的回复出。

#include <cstdint>
#include <string>
#include <vector>

namespace storage {

struct RespReply {
    enum class Type { SimpleString, Error, Integer, BulkString, Array, Nil };
    Type type = Type::Nil;
    std::string str;              // SimpleString / Error / BulkString
    int64_t integer = 0;          // Integer
    std::vector<RespReply> array; // Array

    bool is_error() const { return type == Type::Error; }
    static RespReply simple(std::string s) { return RespReply{Type::SimpleString, std::move(s), 0, {}}; }
};

class RespClient {
public:
    /// 连接失败抛 std::runtime_error。
    RespClient(const std::string& host, uint16_t port);
    ~RespClient();

    RespClient(const RespClient&) = delete;
    RespClient& operator=(const RespClient&) = delete;

    /// 发一条命令并解析回复。IO/协议错误抛 std::runtime_error；
    /// redis 端错误（-ERR）返回 is_error() 的回复。
    RespReply command(const std::vector<std::string>& args);

private:
    void send_command(const std::vector<std::string>& args);
    RespReply parse_reply();
    std::string read_line();          // 读到 \r\n（不含）
    std::string read_bytes(size_t n); // 恰好 n 字节
    char read_byte();
    void fill_buffer();

#ifdef _WIN32
    uintptr_t sock_ = ~uintptr_t(0);  // SOCKET
#else
    int fd_ = -1;
#endif
    std::string buffer_;
};

} // namespace storage

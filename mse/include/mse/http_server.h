#pragma once

// ============================================================================
// mse/http_server.h —— 极简 HTTP/1.1 服务器与客户端(零第三方依赖)
//
// 对外 API 的传输层:POST /events、GET /views/{viewId} 两个动词经
// HttpServer 暴露,业务逻辑全在 ApiGateway(视图即数据,前端无业务逻辑)。
// 原始 socket 实现:Windows(winsock2)/ Linux(posix);单线程 accept 循环
// + 每连接一个处理线程。仅实现本系统需要的子集:Content-Length 请求体、
// keep-alive 关闭、JSON 响应。HTTP 解析器不做通用性承诺。
// ============================================================================

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace mse {

struct HttpRequest {
    std::string method;                         // "GET" | "POST" | ...
    std::string path;                           // 不含查询串,如 "/views/V-REWORK-001"
    std::map<std::string, std::string> query;   // URL 解码后的查询参数
    std::map<std::string, std::string> headers; // 请求头(键统一小写)
    std::string body;
};

struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// 后台线程启动 accept 循环。host 如 "127.0.0.1"。失败返回 false。
    bool listen_on(const std::string& host, uint16_t port, Handler handler);
    void stop();
    uint16_t port() const;  // 实际绑定端口(port=0 时由系统分配)

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// 极简同步 HTTP 客户端(demo/测试用):返回 (status, body);网络错误返回 (-1, 错误信息)。
/// headers:附加请求头(如 {"x-mse-token", "..."};键按原样发送)。
std::pair<int, std::string> http_request(const std::string& host, uint16_t port,
                                         const std::string& method, const std::string& path,
                                         const std::string& body = "",
                                         const std::map<std::string, std::string>& headers = {});

/// URL 查询串解析(百分号解码)。
std::map<std::string, std::string> parse_query(const std::string& query_string);

} // namespace mse

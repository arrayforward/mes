// ============================================================================
// mse/server/main.cpp —— mse_server:人工验证用 HTTP 服务器与只读自省端点
//
// 把 mse 内核(sqlite 后端 + voxelstore 时空持久化 + 全量业务种子)装配成
// 一台长驻 HTTP 服务,供前端/人工联调:
//   写侧  POST /events            变化描述 → 候选回执(X-MSE-Token → 信任级)
//   读侧  GET  /views/{viewId}    视图求值(?observer=&entity=&t=)
//         POST /drain             异步队列手动落账(验证用)
//   自省  GET  /meta/views|event-types|attributes|anchors|ontologies|events
//         (定义层/投影/日志的只读快照,工具性质)
//   静态  GET  / 与 /static/<path>(MSE_WEB_DIR,同源无需 CORS);
//         其余非 API 的 GET 路径也按 web 根静态文件处理(index.html 的
//         相对引用 vendor/、app.js 以 / 为基准解析)
//
// 用法:mse_server [--host 127.0.0.1] [--port 18080] [--db mse_server.db]
//                 [--voxel-db mse_server_voxels.db] [--seed]
//   --seed:即使库非空也强制重放种子定义(定义热更新,append-only 语义)。
//   种子含 SequenceAdjusted 的 WASM 规则接入(R-PLAN-ADJUST-WASM 与
//   jsonlogic 版同一立法、双重拦截,同 demo 第 0 节)。
// Ctrl-C(SIGINT)优雅停止。
// ============================================================================

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "mse/http_server.h"
#include "mse/seeds.h"
#include "mse/system.h"
#include "storage/backends/sqlite_backend.h"
#include "voxelstore/record_voxel_store.h"

#ifdef _WIN32
#include <windows.h>  // GetModuleFileNameA(可移植路径解析)
#endif
#ifdef __linux__
#include <unistd.h>   // readlink(/proc/self/exe)
#endif

using nlohmann::json;

namespace {

// ---- 可移植路径解析(发布包纪律) ----
// 优先编译期钉入的源码路径(开发机);不存在则按 exe 同目录解析
// (发布包布局:mse_server.exe 与 web/、assets/ 同级)。
std::filesystem::path exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (::GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0)
        return std::filesystem::path(buf).parent_path();
#endif
#ifdef __linux__
    char buf[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::filesystem::path(std::string(buf, (size_t)n)).parent_path();
#endif
    return std::filesystem::current_path();
}
std::string resolve_dir(const std::string& compile_time, const char* rel) {
    namespace fs = std::filesystem;
    if (fs::exists(compile_time)) return compile_time;
    const fs::path p = exe_dir() / rel;
    if (fs::exists(p)) return p.string();
    return compile_time;  // 都找不到:返回原值,错误信息保持可诊断
}

// ---- SIGINT:置标志位,主循环轮询后优雅 stop(处理函数内不做系统调用) ----
std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true); }

void print_usage(const char* argv0) {
    std::printf("用法:%s [--host 127.0.0.1] [--port 18080] [--db mse_server.db] "
                "[--voxel-db mse_server_voxels.db] [--seed]\n", argv0);
}

// ---- 静态文件:扩展名 → content-type(仅本系统需要的子集) ----
std::string content_type_of(const std::filesystem::path& p) {
    const std::string ext = p.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js")   return "text/javascript; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".png")  return "image/png";
    return "application/octet-stream";
}

// ---- 静态文件服务:web 根目录下取文件,防 '..' 路径穿越 ----
// web_root 须已 canonical;rel 为 URL 路径去掉前导 '/' 的部分。
mse::HttpResponse serve_static(const std::filesystem::path& web_root,
                               const std::string& rel) {
    // 逐段拒绝 '..' 与反斜杠(Windows 下 '\' 同为分隔符,双管齐下)
    std::istringstream segs(rel);
    std::string seg;
    while (std::getline(segs, seg, '/')) {
        if (seg == ".." || seg.find('\\') != std::string::npos)
            return mse::HttpResponse{403, "application/json; charset=utf-8",
                                     "{\"error\":\"forbidden\"}"};
    }
    // 规范化后再确认目标仍在 web 根目录之内(符号链接等边角)
    const std::filesystem::path target =
        std::filesystem::weakly_canonical(web_root / rel);
    const std::string base = web_root.string();
    const std::string full = target.string();
    if (full.size() < base.size() + 1 || full.compare(0, base.size(), base) != 0 ||
        (full[base.size()] != '/' && full[base.size()] != '\\'))
        return mse::HttpResponse{403, "application/json; charset=utf-8",
                                 "{\"error\":\"forbidden\"}"};

    std::ifstream in(target, std::ios::binary);
    if (!in)
        return mse::HttpResponse{404, "application/json; charset=utf-8",
                                 "{\"error\":\"not found\"}"};
    std::ostringstream body;
    body << in.rdbuf();
    mse::HttpResponse resp{200, content_type_of(target), body.str()};
    // 界面资源禁缓存:本地验证台迭代频繁,陈旧 app.js 会让浏览器跑旧逻辑
    // (服务器无 ETag/304,no-cache 使浏览器每次回源重取)
    resp.headers["Cache-Control"] = "no-cache";
    return resp;
}

json not_found() { return json{{"error", "not found"}}; }

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- 命令行 ----
    uint16_t    port = 18080;              // 默认避开 8080(常被本机服务占用)
    std::string host = "127.0.0.1";  // 默认只绑环回:0.0.0.0 通配绑定会与
                                     // 已占用 127.0.0.1:port 的服务静默共存,
                                     // 浏览器按更具体的绑定路由 → 页面打不开
    std::string db_path = "mse_server.db";
    std::string voxel_db_path = "mse_server_voxels.db";
    bool        force_seed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺值\n", name);
                print_usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") {
            host = need_value("--host");
        } else if (a == "--port") {
            port = static_cast<uint16_t>(std::atoi(need_value("--port")));
        } else if (a == "--db") {
            db_path = need_value("--db");
        } else if (a == "--voxel-db") {
            voxel_db_path = need_value("--voxel-db");
        } else if (a == "--seed") {
            force_seed = true;
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数:%s\n", a.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    // ---- 装配:sqlite 后端 + voxelstore 时空持久化 + System(同 demo 第 0 节) ----
    storage::SqliteBackend backend(db_path);  // storage 是唯一持久化接口
    auto voxel_backend = std::make_unique<storage::SqliteBackend>(voxel_db_path);
    voxelstore::RecordVoxelStore voxel_store(std::move(voxel_backend));
    mse::System sys(backend, &voxel_store);

    // ---- 种子:库为空(属性字典无键)或 --seed 时载入 ----
    const std::string assets_dir = resolve_dir(MSE_ASSETS_DIR, "assets");
    const bool empty = sys.defs().attr_keys().empty();
    if (empty || force_seed) {
        mse::load_system_keys(sys.defs());        // 一切皆属性:系统键先登记
        mse::load_auto_plant_seeds(sys.defs());   // 词典/行为/规则/视图(46 视图)
        mse::load_wasm_seeds(sys.defs(), assets_dir);  // WASM 规则沙盒
        // WASM 规则接入类型(与 demo 第 0 节同一立法):SequenceAdjusted 双拦截
        const json wasm_wire = sys.api().post_definition(
            "EventTypeRegistered",
            {{"type", "SequenceAdjusted"},
             {"required_keys", {"id", "actor"}},
             {"optional_keys", {"优先级", "序列号"}},
             {"rules", {"R-PLAN-ADJUST", "R-SEQ-FROZEN", "R-PLAN-ADJUST-WASM"}},
             {"correction", "SequenceAdjustReversed"},
             {"multi_target", false},
             {"settlement", "sync"}});
        if (wasm_wire.value("status", "") != "settled") {
            std::fprintf(stderr, "WASM 规则接入 SequenceAdjusted 失败:%s\n",
                         wasm_wire.dump().c_str());
            return 1;
        }
    }
    sys.knowledge().load(assets_dir + "/ontology_seed.json",
                         assets_dir + "/lexicon.json");

    // 信任分级:凭证 → 信任级(HTTP 头 X-MSE-Token 查表;未携带/未登记 → 0)
    sys.api().set_trust_tokens({{"ui-token", 2}, {"plc-gw-token", 1}, {"erp-token", 1}});

    // ---- 启动横幅 ----
    std::printf("============================================================\n");
    std::printf("mse_server —— 车间世界模型人工验证服务器\n");
    std::printf("  端口:%u\n", port);
    std::printf("  业务库:%s\n  时空库:%s\n", db_path.c_str(), voxel_db_path.c_str());
    std::printf("  种子:%s(属性 %zu 键,事件类型 %zu 个,规则 %zu 条,视图 %zu 个,锚点 %zu 个)\n",
                empty ? "已载入(空库)" : (force_seed ? "强制重放" : "沿用库内定义"),
                sys.defs().attr_keys().size(), sys.defs().event_type_names().size(),
                sys.defs().rules().size(), sys.defs().views_all().size(),
                sys.defs().anchors().size());
    std::printf("  事件日志:%lld 条  knowledge 协助:%s\n",
                static_cast<long long>(sys.log().size()),
                sys.knowledge().loaded() ? "已加载" : "未加载");
    std::printf("  凭证(X-MSE-Token):ui-token(L2) / plc-gw-token(L1) / erp-token(L1)\n");
    std::printf("============================================================\n");

    // ---- 静态资源根(编译期路径优先,发布包按 exe 同目录 web/ 解析) ----
    const std::filesystem::path web_root =
        std::filesystem::weakly_canonical(resolve_dir(MSE_WEB_DIR, "web"));

    // 写侧管线是单写者:所有请求经一把互斥锁串行化(验证服务器,简单优先)
    std::mutex mu;

    mse::HttpServer server;
    const bool listening =
        server.listen_on(host, port, [&](const mse::HttpRequest& req) {
            std::lock_guard<std::mutex> lock(mu);

            // 写动词:POST /events(凭证头 → 信任级)
            if (req.method == "POST" && req.path == "/events") {
                const json payload = json::parse(req.body, nullptr, false);
                if (payload.is_discarded())
                    return mse::HttpResponse{400, "application/json; charset=utf-8",
                                             "{\"error\":\"bad json\"}"};
                int trust = 0;
                if (auto it = req.headers.find("x-mse-token"); it != req.headers.end())
                    trust = sys.api().trust_of(it->second);
                return mse::HttpResponse{200, "application/json; charset=utf-8",
                                         sys.api().post_events(payload, trust).dump()};
            }

            // 异步结算边界:手动 drain(验证用)
            if (req.method == "POST" && req.path == "/drain") {
                const size_t n = sys.pipeline().drain_async();
                return mse::HttpResponse{200, "application/json; charset=utf-8",
                                         json{{"drained", n}}.dump()};
            }

            if (req.method == "GET") {
                // 读动词:GET /views/{viewId}(先判 /meta/,再 /views/,前缀不重叠)
                if (req.path.rfind("/views/", 0) == 0) {
                    return mse::HttpResponse{
                        200, "application/json; charset=utf-8",
                        sys.api().get_view(req.path.substr(7), req.query).dump()};
                }

                // ---- 自省端点(定义层/投影/日志的只读快照) ----
                if (req.path == "/meta/views") {
                    json arr = json::array();
                    for (const auto& [id, e] : sys.defs().views_all())
                        arr.push_back(e);  // to_json(ViewEntry)
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }
                if (req.path == "/meta/event-types") {
                    json arr = json::array();
                    for (const auto& [name, e] : sys.defs().types_all())
                        arr.push_back(e);  // to_json(EventTypeEntry)
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }
                if (req.path == "/meta/attributes") {
                    json arr = json::array();
                    for (const auto& [key, e] : sys.defs().attrs_all())
                        arr.push_back(e);  // to_json(AttributeEntry)
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }
                if (req.path == "/meta/anchors") {
                    json arr = json::array();
                    for (const auto& [path, e] : sys.defs().anchors())
                        arr.push_back(json{{"path", path}, {"name", e.name}});
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }
                if (req.path == "/meta/ontologies") {
                    // 全部本体 id + 属性键清单(不含值;值走视图)
                    json arr = json::array();
                    for (const mse::Ontology* o : sys.projection().ontologies()) {
                        json keys = json::array();
                        for (const auto& [k, cell] : o->attrs) keys.push_back(k);
                        arr.push_back(json{{"id", o->id}, {"keys", keys}});
                    }
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }
                if (req.path == "/meta/events") {
                    // 最近 N 条事件,新的在前;N 上限 500
                    int64_t limit = 50;
                    if (auto it = req.query.find("limit"); it != req.query.end()) {
                        try {
                            limit = std::stoll(it->second);
                        } catch (...) {
                            limit = 50;
                        }
                    }
                    if (limit < 1) limit = 1;
                    if (limit > 500) limit = 500;
                    const auto& all = sys.log().all();
                    json arr = json::array();
                    for (auto it = all.rbegin();
                         it != all.rend() && limit-- > 0; ++it)
                        arr.push_back(*it);  // to_json(Event)
                    return mse::HttpResponse{200, "application/json; charset=utf-8",
                                             arr.dump()};
                }

                // ---- 静态资源(同源,无需 CORS) ----
                // index.html 用相对路径引用 vendor/*.js 与 app.js;页面基准是 /,
                // 浏览器会请求 /vendor/...、/app.js —— 故除上述 API 路径外的
                // GET 一律按 web 根静态文件处理(不存在即 404)。
                if (req.path == "/")
                    return serve_static(web_root, "index.html");
                if (req.path.rfind("/static/", 0) == 0)
                    return serve_static(web_root, req.path.substr(8));
                return serve_static(web_root, req.path.substr(1));
            }

            return mse::HttpResponse{404, "application/json; charset=utf-8",
                                     not_found().dump()};
        });
    if (!listening) {
        std::fprintf(stderr, "HTTP 服务监听失败(端口 %u)\n", port);
        return 1;
    }
    std::printf("HTTP 服务就绪:http://127.0.0.1:%u/(Ctrl-C 停止)\n", server.port());

    // ---- 阻塞运行直到 SIGINT ----
    std::signal(SIGINT, on_sigint);
    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("\n收到 SIGINT,正在停止……\n");
    server.stop();
    std::printf("已停止。\n");
    return 0;
}

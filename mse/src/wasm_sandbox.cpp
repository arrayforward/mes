// ============================================================================
// mse/wasm_sandbox.cpp —— WASM 规则沙盒(wasm3)+ 烘焙管线(实现)
//
// 实现 include/mse/wasm_sandbox.h 注释里逐字定义的 WASM ABI:
//   宿主导入模块 "mse":attr_len/attr_get/write_len/write_get/result。
//   最小权限在 ABI 层执行:deps 之外的键一律返回 -1"不存在"。
//   无环境:不链接 WASI/时钟/随机/文件,且烘焙静态闸拒绝任何白名单外导入。
// 模块缓存:artifact_hash → 预编译实例(单写者串行调用,无锁)。
// ============================================================================

#include "mse/wasm_sandbox.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>

#include <wasm3.h>
#include <m3_env.h>

namespace mse {

// ---- base64(产物进定义事件负载用) ----
namespace {

const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64_encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = static_cast<uint8_t>(bytes[i]);
        const uint32_t b1 = (i + 1 < bytes.size()) ? static_cast<uint8_t>(bytes[i + 1]) : 0;
        const uint32_t b2 = (i + 2 < bytes.size()) ? static_cast<uint8_t>(bytes[i + 2]) : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kB64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < bytes.size() ? kB64Alphabet[triple & 0x3F] : '=');
    }
    return out;
}

std::string base64_decode(const std::string& b64, bool& ok) {
    ok = false;
    if (b64.size() % 4 != 0) return {};
    std::string out;
    out.reserve(b64.size() / 4 * 3);
    for (size_t i = 0; i < b64.size(); i += 4) {
        uint32_t vals[4];
        int      pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = b64[i + static_cast<size_t>(k)];
            if (c == '=') {
                if (i + 4 != b64.size() || k < 2) return {};  // '=' 只允许在末组尾部
                vals[k] = 0;
                ++pad;
            } else {
                if (pad > 0) return {};  // '=' 后还有有效字符
                const int v = b64_value(c);
                if (v < 0) return {};
                vals[k] = static_cast<uint32_t>(v);
            }
        }
        const uint32_t triple = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6) | vals[3];
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(triple & 0xFF));
    }
    ok = true;
    return out;
}

// ---- FNV-1a 64(产物内容寻址哈希,16 位 hex 小写) ----
namespace {

std::string fnv1a64_hex(const std::string& bytes) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// ---- 宿主闭包:每次 eval 前换绑,raw 导入函数经 _ctx->userdata 拿到 ----
struct HostCtx {
    std::set<std::string> deps;     // 最小权限白名单
    const json*           attrs = nullptr;   // 注入属性(只含 deps 键)
    const json*           writes = nullptr;  // 候选对当前目标本体的写入
    std::string           result_kind;
    std::string           result_payload;
    bool                  result_set = false;
};

HostCtx* host_ctx(IM3ImportContext ctx) {
    return static_cast<HostCtx*>(ctx->userdata);
}

// 读 guest 线性内存中的字节串;越界 → false(调用点 trap)。
// _mem 即数据段基址(m3MemData);不用 m3ApiIsNullPtr——它把 offset 0 当 null,
// 而 ABI 不保留 null 指针约定,地址 0 是合法数据。
bool guest_read(void* _mem, IM3Runtime runtime, uint32_t ptr, uint32_t len, std::string& out) {
    if (static_cast<uint64_t>(ptr) + len > m3_GetMemorySize(runtime)) return false;
    out.assign(static_cast<const char*>(_mem) + ptr, len);
    return true;
}

// 写 guest 线性内存;越界 → false(调用点 trap)
bool guest_write(void* _mem, IM3Runtime runtime, uint32_t ptr, const std::string& data) {
    if (static_cast<uint64_t>(ptr) + data.size() > m3_GetMemorySize(runtime)) return false;
    if (!data.empty()) std::memcpy(static_cast<char*>(_mem) + ptr, data.data(), data.size());
    return true;
}

// 键 → JSON 文本值。key ∉ deps → false(返回 -1"不存在");∈ deps 但无值或
// 值为 null → 同样 false:ABI 层 null == 缺失(与 JSON-logic 侧 var 缺失
// 得 null、规则按"无值"处理同一口径——双运行时语义一致)。
bool lookup_json_text(const std::string& key, const HostCtx& hc, const json* bag,
                      std::string& out) {
    if (hc.deps.count(key) == 0) return false;
    json v = nullptr;
    if (bag != nullptr && bag->is_object() && bag->contains(key)) v = bag->at(key);
    if (v.is_null()) return false;
    out = v.dump();
    return true;
}

} // namespace

// ---- 宿主导入(模块 "mse",全部确定性纯函数) ----

// attr_len(key_ptr,key_len) → 值 JSON 文本长度;不存在/未声明 → -1
static m3ApiRawFunction(host_attr_len) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, key_len)
    std::string key;
    if (!guest_read(_mem, runtime, key_ptr, key_len, key)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    std::string text;
    if (!lookup_json_text(key, *host_ctx(_ctx), host_ctx(_ctx)->attrs, text)) m3ApiReturn(-1);
    m3ApiReturn(static_cast<int32_t>(text.size()));
}

// attr_get(key_ptr,key_len,out_ptr,out_cap) → 实际长度;容量不足 → -2
static m3ApiRawFunction(host_attr_get) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, key_len)
    m3ApiGetArg(uint32_t, out_ptr)
    m3ApiGetArg(uint32_t, out_cap)
    std::string key;
    if (!guest_read(_mem, runtime, key_ptr, key_len, key)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    std::string text;
    if (!lookup_json_text(key, *host_ctx(_ctx), host_ctx(_ctx)->attrs, text)) m3ApiReturn(-1);
    if (text.size() > out_cap) m3ApiReturn(-2);
    if (!guest_write(_mem, runtime, out_ptr, text)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    m3ApiReturn(static_cast<int32_t>(text.size()));
}

// write_len(key_ptr,key_len):候选对当前目标本体写入的新值
static m3ApiRawFunction(host_write_len) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, key_len)
    std::string key;
    if (!guest_read(_mem, runtime, key_ptr, key_len, key)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    std::string text;
    if (!lookup_json_text(key, *host_ctx(_ctx), host_ctx(_ctx)->writes, text)) m3ApiReturn(-1);
    m3ApiReturn(static_cast<int32_t>(text.size()));
}

// write_get(key_ptr,key_len,out_ptr,out_cap)
static m3ApiRawFunction(host_write_get) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, key_ptr)
    m3ApiGetArg(uint32_t, key_len)
    m3ApiGetArg(uint32_t, out_ptr)
    m3ApiGetArg(uint32_t, out_cap)
    std::string key;
    if (!guest_read(_mem, runtime, key_ptr, key_len, key)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    std::string text;
    if (!lookup_json_text(key, *host_ctx(_ctx), host_ctx(_ctx)->writes, text)) m3ApiReturn(-1);
    if (text.size() > out_cap) m3ApiReturn(-2);
    if (!guest_write(_mem, runtime, out_ptr, text)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    m3ApiReturn(static_cast<int32_t>(text.size()));
}

// result(kind_ptr,kind_len,payload_ptr,payload_len):规则结论(多次调用末次生效)
static m3ApiRawFunction(host_result) {
    m3ApiGetArg(uint32_t, kind_ptr)
    m3ApiGetArg(uint32_t, kind_len)
    m3ApiGetArg(uint32_t, payload_ptr)
    m3ApiGetArg(uint32_t, payload_len)
    HostCtx* hc = host_ctx(_ctx);
    if (!guest_read(_mem, runtime, kind_ptr, kind_len, hc->result_kind) ||
        !guest_read(_mem, runtime, payload_ptr, payload_len, hc->result_payload)) {
        m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess);
    }
    hc->result_set = true;
    m3ApiSuccess();
}

namespace {

// 宿主 ABI 白名单:模块声明的导入函数集合必须 ⊆ 此表("无环境"的静态闸)
const std::set<std::pair<std::string, std::string>>& import_whitelist() {
    static const std::set<std::pair<std::string, std::string>> kAllowed = {
        {"mse", "attr_len"}, {"mse", "attr_get"}, {"mse", "write_len"},
        {"mse", "write_get"}, {"mse", "result"},
    };
    return kAllowed;
}

// ---- 预编译模块实例(缓存单元):wasm 字节须与模块同生命周期 ----
struct Baked {
    std::string    bytes;         // m3_ParseModule 要求字节缓冲持久
    IM3Environment env = nullptr;
    IM3Runtime     runtime = nullptr;
    IM3Function    entry = nullptr;
    HostCtx        host;          // 链接时钉进 userdata;每次 eval 前换绑字段

    Baked() = default;
    Baked(const Baked&) = delete;
    Baked& operator=(const Baked&) = delete;
    ~Baked() {
        if (runtime != nullptr) m3_FreeRuntime(runtime);  // 随 runtime 释放 module
        if (env != nullptr) m3_FreeEnvironment(env);
    }
};

// 检查导入白名单(任何其他导入 = 环境能力伸手,拒绝)
std::string check_imports(IM3Module mod) {
    for (u32 i = 0; i < mod->numFunctions; ++i) {
        const M3Function& f = mod->functions[i];
        if (f.import.moduleUtf8 == nullptr || f.import.fieldUtf8 == nullptr) continue;
        const std::pair<std::string, std::string> imp = {f.import.moduleUtf8, f.import.fieldUtf8};
        if (import_whitelist().count(imp) == 0) {
            return "模块声明了白名单外的环境导入: " + imp.first + "." + imp.second;
        }
    }
    return {};
}

// 链接 5 个宿主导入;模块未声明某个导入时 wasm3 返回 functionLookupFailed,忽略
std::string link_host_imports(IM3Module mod, HostCtx* hc) {
    struct Binding {
        const char* name;
        const char* sig;
        M3RawCall   fn;
    };
    static const Binding kBindings[] = {
        {"attr_len",  "i(ii)",   &host_attr_len},
        {"attr_get",  "i(iiii)", &host_attr_get},
        {"write_len", "i(ii)",   &host_write_len},
        {"write_get", "i(iiii)", &host_write_get},
        {"result",    "v(iiii)", &host_result},
    };
    for (const Binding& b : kBindings) {
        M3Result r = m3_LinkRawFunctionEx(mod, "mse", b.name, b.sig, b.fn, hc);
        if (r != nullptr && r != m3Err_functionLookupFailed) {
            return std::string("链接宿主导入失败 mse.") + b.name + ": " + r;
        }
    }
    return {};
}

// 实例化:解析 → 导入静态闸 → 加载 → 链接 → 定位导出。失败返回原因。
std::string instantiate(Baked& b, const std::string& export_name) {
    b.env = m3_NewEnvironment();
    if (b.env == nullptr) return "m3_NewEnvironment 失败";
    b.runtime = m3_NewRuntime(b.env, 64 * 1024, nullptr);
    if (b.runtime == nullptr) return "m3_NewRuntime 失败";

    IM3Module mod = nullptr;
    if (M3Result r = m3_ParseModule(b.env, &mod,
                                    reinterpret_cast<const uint8_t*>(b.bytes.data()),
                                    static_cast<uint32_t>(b.bytes.size()))) {
        return std::string("WASM 产物解析失败: ") + r;
    }
    if (std::string err = check_imports(mod); !err.empty()) {
        m3_FreeModule(mod);  // 尚未 LoadModule,所有权仍在调用方
        return err;
    }
    if (M3Result r = m3_LoadModule(b.runtime, mod)) {
        const std::string err = std::string("WASM 模块加载失败: ") + r;
        m3_FreeModule(mod);  // LoadModule 失败,所有权未转移
        return err;
    }
    // LoadModule 成功后 module 归 runtime 管
    if (std::string err = link_host_imports(mod, &b.host); !err.empty()) return err;

    if (M3Result r = m3_FindFunction(&b.entry, b.runtime, export_name.c_str())) {
        return "模块缺少入口导出 " + export_name + ": " + r;
    }
    if (m3_GetArgCount(b.entry) != 0) {
        return "入口导出 " + export_name + " 须为无参函数(ABI 约定)";
    }
    return {};
}

// 试跑/求值共用:调入口函数,trap → 错误描述
std::string call_entry(Baked& b) {
    M3ErrorInfo info;
    m3_ResetErrorInfo(b.runtime);
    if (M3Result r = m3_CallV(b.entry)) {
        std::string msg = std::string("WASM 执行 trap: ") + r;
        m3_GetErrorInfo(b.runtime, &info);
        if (info.message != nullptr) msg += std::string(" (") + info.message + ")";
        return msg;
    }
    return {};
}

// 模块缓存:artifact_hash → 预编译实例(单写者串行调用,无锁)
std::unordered_map<std::string, std::shared_ptr<Baked>>& module_cache() {
    static std::unordered_map<std::string, std::shared_ptr<Baked>> cache;
    return cache;
}

} // namespace

std::string WasmSandbox::engine_version() {
    return std::string("wasm3 ") + M3_VERSION;
}

// ----------------------------------------------------------------------------
// bake:解码 → 静态检查(deps 已登记、导出存在、无环境导入)→ 沙盒试跑 → 钉版本
// ----------------------------------------------------------------------------
WasmSandbox::BakeResult WasmSandbox::bake(
    const std::string& artifact_b64, const std::vector<std::string>& deps,
    const std::string& export_name,
    const std::function<bool(const std::string&)>& is_registered) {
    BakeResult out;

    bool ok = false;
    const std::string bytes = base64_decode(artifact_b64, ok);
    if (!ok || bytes.empty()) {
        out.errors.push_back("artifact 不是合法的 base64 产物");
        return out;
    }

    // ① deps 每键已登记(引用完整性,与 jsonlogic 静态检查同纪律)
    for (const auto& d : deps) {
        if (!is_registered(d)) out.errors.push_back("deps 引用未登记键: " + d);
    }
    if (!out.errors.empty()) return out;

    Baked b;
    b.bytes = bytes;
    // ②③ 实例化内含:无环境导入静态闸 + 入口导出存在性
    if (std::string err = instantiate(b, export_name); !err.empty()) {
        out.errors.push_back(err);
        return out;
    }

    // ④ 沙盒试跑:样例 envelope(deps 全 null 的 attrs、空候选写入)
    json sample_attrs = json::object();
    for (const auto& d : deps) sample_attrs[d] = nullptr;
    static const json kEmptyWrites = json::object();
    b.host.deps = std::set<std::string>(deps.begin(), deps.end());
    b.host.attrs = &sample_attrs;
    b.host.writes = &kEmptyWrites;
    b.host.result_set = false;
    b.host.result_kind.clear();
    b.host.result_payload.clear();
    // 规则在样例下给出何种结论(reject/noop 等)不强求;trap 才算烘焙失败
    if (std::string err = call_entry(b); !err.empty()) {
        out.errors.push_back("沙盒试跑失败: " + err);
        return out;
    }

    out.ok = true;
    out.artifact_hash = fnv1a64_hex(bytes);
    out.engine_version = engine_version();
    return out;
}

// ----------------------------------------------------------------------------
// eval:加载/缓存 → 换绑闭包 → 调导出 → result 翻译为 RuleOutcome
// ----------------------------------------------------------------------------
RuleOutcome WasmSandbox::eval(const Rule& r, const json& attrs, const Candidate& candidate,
                              const std::string& target_id) const {
    bool ok = false;
    const std::string bytes = base64_decode(r.artifact, ok);
    if (!ok || bytes.empty()) {
        throw RuleError("规则 " + r.rule_id + ": artifact 不是合法的 base64 产物");
    }
    const std::string hash = fnv1a64_hex(bytes);

    // 加载/缓存(内容寻址:同产物同实例)
    std::shared_ptr<Baked> baked;
    auto it = module_cache().find(hash);
    if (it != module_cache().end()) {
        baked = it->second;
    } else {
        baked = std::make_shared<Baked>();
        baked->bytes = bytes;
        if (std::string err = instantiate(*baked, r.export_name); !err.empty()) {
            throw RuleError("规则 " + r.rule_id + ": " + err);
        }
        module_cache().emplace(hash, baked);
    }

    // 换绑宿主闭包(最小权限:deps 白名单 + 注入属性 + 目标本体候选写入)
    auto wit = candidate.writes.find(target_id);
    const json* writes =
        (wit != candidate.writes.end() && wit->second.is_object()) ? &wit->second : nullptr;
    HostCtx& hc = baked->host;
    hc.deps = std::set<std::string>(r.deps.begin(), r.deps.end());
    hc.attrs = &attrs;
    hc.writes = writes;
    hc.result_set = false;
    hc.result_kind.clear();
    hc.result_payload.clear();

    if (std::string err = call_entry(*baked); !err.empty()) {
        throw RuleError("规则 " + r.rule_id + ": " + err);
    }

    // result 收集 → RuleOutcome
    RuleOutcome out;
    if (!hc.result_set || hc.result_kind == "noop") {
        out = RuleOutcome::noop();
    } else if (hc.result_kind == "pass") {
        out = RuleOutcome::pass();
    } else if (hc.result_kind == "reject") {
        out = RuleOutcome::reject(hc.result_payload);
    } else if (hc.result_kind == "value") {
        json payload;
        try {
            payload = hc.result_payload.empty() ? json(nullptr) : json::parse(hc.result_payload);
        } catch (const std::exception& e) {
            throw RuleError("规则 " + r.rule_id + ": result payload 不是合法 JSON: " + e.what());
        }
        if (payload.is_object() && payload.contains("emit")) {
            // 触发(emit)结论:{"emit":[候选扁平载荷,...]} —— 与 rules.cpp 同一约定
            if (!payload.at("emit").is_array()) {
                throw RuleError("规则 " + r.rule_id + ": emit 载荷须为数组");
            }
            static const std::set<std::string> kSysKeys = {
                "type", "id", "actor", "writes", "occur_time", "space",
                "evidence", "corrects", "idempotency_key",
            };
            std::vector<Candidate> emitted;
            for (const auto& item : payload.at("emit")) {
                if (!item.is_object()) throw RuleError("规则 " + r.rule_id + ": emit 候选须为 object");
                if (!item.contains("type") || !item.at("type").is_string() ||
                    item.at("type").get<std::string>().empty()) {
                    throw RuleError("规则 " + r.rule_id + ": emit 载荷缺少 type(非空字符串)");
                }
                Candidate c;
                c.type = item.at("type").get<std::string>();
                if (item.contains("writes")) {
                    if (!item.at("writes").is_object()) {
                        throw RuleError("规则 " + r.rule_id + ": emit 载荷 writes 须为 object");
                    }
                    for (const auto& [wid, wobj] : item.at("writes").items()) c.writes[wid] = wobj;
                }
                if (item.contains("id")) {
                    if (!item.at("id").is_string()) {
                        throw RuleError("规则 " + r.rule_id + ": emit 载荷 id 须为字符串");
                    }
                    const std::string wid = item.at("id").get<std::string>();
                    json w = json::object();
                    auto   wit2 = c.writes.find(wid);
                    if (wit2 != c.writes.end() && wit2->second.is_object()) w = wit2->second;
                    for (const auto& [k, v] : item.items()) {
                        if (kSysKeys.count(k) == 0) w[k] = v;
                    }
                    c.writes[wid] = std::move(w);
                }
                if (c.writes.empty()) {
                    throw RuleError("规则 " + r.rule_id + ": emit 载荷须含 id 或 writes 之一");
                }
                // actor 缺省 "rule:<rule_id>";space/occur_time 由管线继承父候选
                c.actor = (item.contains("actor") && item.at("actor").is_string() &&
                           !item.at("actor").get<std::string>().empty())
                              ? item.at("actor").get<std::string>()
                              : ("rule:" + r.rule_id);
                emitted.push_back(std::move(c));
            }
            out = RuleOutcome::emit(std::move(emitted));
        } else {
            out = RuleOutcome::value_of(std::move(payload));
        }
    } else {
        throw RuleError("规则 " + r.rule_id + ": result kind 非法(须为 pass/reject/value/noop): " +
                        hc.result_kind);
    }

    // effect 终态种类校验(与 rules.cpp 同一纪律)
    using K = RuleOutcome::Kind;
    if (r.effect == "filter" && out.kind != K::kPass && out.kind != K::kReject) {
        throw RuleError("规则 " + r.rule_id + ": filter 规则结果须为 pass/reject");
    }
    if (r.effect == "derive" && out.kind != K::kValue) {
        throw RuleError("规则 " + r.rule_id + ": derive 规则结果须为 return");
    }
    if (r.effect == "trigger" && out.kind != K::kEmit && out.kind != K::kNoop) {
        throw RuleError("规则 " + r.rule_id + ": trigger 规则结果须为 emit/noop");
    }
    return out;
}

} // namespace mse

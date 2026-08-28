// ============================================================================
// 文件: persistence.cpp
// 模块: stmb_persistence(本地文件持久化,依赖 core / history)
// 用途: 实现 PersistenceManager 的快照与 WAL 读写,以及全部二进制编解码。
// 设计思路:
//   1. 最底层是 ByteWriter / ByteReader 两个小工具,所有整数显式按小端序
//      逐字节读写,double 按位拷贝为 u64,optional 用 1 字节存在标志,
//      保证格式与平台无关;
//   2. CRC32 用标准多项式 0xEDB88320 的表驱动实现,表在编译期生成;
//   3. 快照与 WAL 共用同一套「文件头 + 记录帧」解析器 parseFrames,
//      解析器天然容错:CRC 不匹配或尾部截断时停止并保留已验证前缀,
//      由调用方决定这种前缀是否可接受(WAL 可接受,快照要求完整);
//   4. 快照写入走「临时文件 + rename」保证原子性,读到的快照要么完整要么
//      不存在,因此对快照的截断/CRC 错误一律按损坏处理(返回空 optional);
//   5. WAL 追加时若文件不存在/为空,先落文件头,保证 replay 总能校验 magic。
// 架构角色: PersistenceManager 的唯一实现文件。
// ============================================================================
#include "persistence.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace stmb {
namespace {

constexpr std::uint32_t kMagic = 0x53544D42U;     // 文件魔数 'STMB'
constexpr std::uint32_t kFormatVersion = 6U;      // 格式版本(v6:来源注册表落盘)
constexpr std::size_t   kHeaderSize = 8U;         // magic u32 + version u32
constexpr std::size_t   kFrameHeaderSize = 5U;    // type u8 + payloadLen u32
constexpr std::size_t   kCrcSize = 4U;            // crc32 u32

// ---------------------------------------------------------------------------
// CRC32(标准多项式 0xEDB88320,表驱动)
// ---------------------------------------------------------------------------

// 伪代码:
//   1. 对 0..255 每个字节值,做 8 轮「低位为 1 则异或多项式再右移」;
//   2. 结果填入 256 项查表数组,编译期完成。
constexpr std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}

// 伪代码:
//   1. crc 初始化为 0xFFFFFFFF;
//   2. 逐字节查表更新 crc;
//   3. 末尾按位取反返回。
std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    static constexpr std::array<std::uint32_t, 256> table = makeCrcTable();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

// ---------------------------------------------------------------------------
// 字节流读写工具(显式小端序)
// ---------------------------------------------------------------------------

// 伪代码(写入器):每个 putXxx 把值拆成小端字节序列追加到缓冲区;
//   字符串先写 u32 长度再写字节;double 按位拷贝为 u64 后按 u64 写。
class ByteWriter {
public:
    void putU8(std::uint8_t v) { buf_.push_back(v); }

    void putU32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFU));
        }
    }

    void putU64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFU));
        }
    }

    void putI64(std::int64_t v) { putU64(static_cast<std::uint64_t>(v)); }

    void putDouble(double v) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(v), "double 必须为 64 位");
        std::memcpy(&bits, &v, sizeof(v));
        putU64(bits);
    }

    void putString(const std::string& s) {
        putU32(static_cast<std::uint32_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
    }

    void putBytes(const std::vector<std::uint8_t>& bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<std::uint8_t>& bytes() const { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

// 伪代码(读取器):每个 getXxx 先检查剩余字节数,不足则置 ok_=false 并
//   返回零值;调用方在解码完成后检查 ok() 判定是否截断。
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    std::uint8_t getU8() {
        if (remaining() < 1) { ok_ = false; return 0; }
        return data_[pos_++];
    }

    std::uint32_t getU32() {
        if (remaining() < 4) { ok_ = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data_[pos_++]) << (i * 8);
        }
        return v;
    }

    std::uint64_t getU64() {
        if (remaining() < 8) { ok_ = false; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data_[pos_++]) << (i * 8);
        }
        return v;
    }

    std::int64_t getI64() { return static_cast<std::int64_t>(getU64()); }

    double getDouble() {
        const std::uint64_t bits = getU64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::string getString() {
        const std::uint32_t len = getU32();
        if (!ok_ || remaining() < len) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    bool ok() const { return ok_; }
    std::size_t remaining() const { return size_ - pos_; }

    // 跳过 n 个字节(用于整段跳过已单独取出的 payload)
    void skip(std::size_t n) {
        if (remaining() < n) { ok_ = false; return; }
        pos_ += n;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// MemoryBlock / BlockVersion / WalRecord 编解码
// ---------------------------------------------------------------------------

// 伪代码:
//   1. 按固定顺序写入块的全部字段(含状态机与置信度字段);
//   2. 两个 optional 候选字段各写 1 字节存在标志,存在再写内容;
//   3. 返回序列化字节。
std::vector<std::uint8_t> encodeBlock(const MemoryBlock& b) {
    ByteWriter w;
    w.putU64(b.id);
    w.putI64(b.key.cellX);
    w.putI64(b.key.cellY);
    w.putI64(b.key.cellZ);
    w.putI64(b.key.timeSlot);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(b.region.min[axis]);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(b.region.max[axis]);
    }
    w.putString(b.payload);
    w.putI64(b.timestamp);
    w.putU32(b.version);
    w.putI64(b.lastAccess);
    w.putU8(static_cast<std::uint8_t>(b.state));
    w.putDouble(b.confidence);
    w.putDouble(b.confirmations);  // v5:加权确认计数(double)
    w.putI64(b.lastUpdate);
    w.putU8(b.pendingPayload.has_value() ? 1 : 0);
    if (b.pendingPayload.has_value()) {
        w.putString(*b.pendingPayload);
    }
    w.putU8(b.pendingConfidence.has_value() ? 1 : 0);
    if (b.pendingConfidence.has_value()) {
        w.putDouble(*b.pendingConfidence);
    }
    // v3 新增:LOD 层级字段(hasFinerData 为查询期瞬时值,不落盘)
    w.putU32(static_cast<std::uint32_t>(b.level));
    w.putU8(b.isSummary ? 1 : 0);
    w.putU32(static_cast<std::uint32_t>(b.sourceLevel));
    // v5 新增:观测管线字段(suspect / 观察窗口 / 周期模式;
    //   hasFinerData 与 temporarilyOccupiedBy 为查询期瞬时值,不落盘)
    w.putU8(b.suspect ? 1 : 0);
    w.putI64(b.windowStart);
    w.putDouble(b.windowWeight);
    w.putU32(static_cast<std::uint32_t>(b.windowSources.size()));
    for (SourceId src : b.windowSources) {
        w.putU64(src);
    }
    w.putU8(b.pattern.has_value() ? 1 : 0);
    if (b.pattern.has_value()) {
        w.putI64(b.pattern->periodMs);
        w.putU32(static_cast<std::uint32_t>(b.pattern->phases.size()));
        for (const PeriodicPattern::Phase& phase : b.pattern->phases) {
            w.putI64(phase.offsetMs);
            w.putI64(phase.durationMs);
            w.putString(phase.payload);
        }
    }
    return w.bytes();
}

// 伪代码:
//   1. 按 encodeBlock 相同的顺序逐字段读出;
//   2. 全部读完后由调用方检查 reader.ok() 判定是否截断。
void decodeBlock(ByteReader& r, MemoryBlock& b) {
    b.id = r.getU64();
    b.key.cellX = r.getI64();
    b.key.cellY = r.getI64();
    b.key.cellZ = r.getI64();
    b.key.timeSlot = r.getI64();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        b.region.min[axis] = r.getDouble();
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        b.region.max[axis] = r.getDouble();
    }
    b.payload = r.getString();
    b.timestamp = r.getI64();
    b.version = r.getU32();
    b.lastAccess = r.getI64();
    b.state = static_cast<BlockState>(r.getU8());
    b.confidence = r.getDouble();
    b.confirmations = r.getDouble();  // v5:加权确认计数(double)
    b.lastUpdate = r.getI64();
    if (r.getU8() != 0) {
        b.pendingPayload = r.getString();
    }
    if (r.getU8() != 0) {
        b.pendingConfidence = r.getDouble();
    }
    // v3 新增:LOD 层级字段
    b.level = static_cast<int>(r.getU32());
    b.isSummary = (r.getU8() != 0);
    b.sourceLevel = static_cast<int>(r.getU32());
    // v5 新增:观测管线字段
    b.suspect = (r.getU8() != 0);
    b.windowStart = r.getI64();
    b.windowWeight = r.getDouble();
    const std::uint32_t windowSourceCount = r.getU32();
    for (std::uint32_t i = 0; i < windowSourceCount && r.ok(); ++i) {
        b.windowSources.insert(r.getU64());
    }
    if (r.getU8() != 0) {
        PeriodicPattern pattern;
        pattern.periodMs = r.getI64();
        const std::uint32_t phaseCount = r.getU32();
        for (std::uint32_t i = 0; i < phaseCount && r.ok(); ++i) {
            PeriodicPattern::Phase phase;
            phase.offsetMs = r.getI64();
            phase.durationMs = r.getI64();
            phase.payload = r.getString();
            pattern.phases.push_back(std::move(phase));
        }
        b.pattern = std::move(pattern);
    }
}

// 伪代码:
//   1. 依次写入 version / payload / confidence / state / validFrom;
//   2. validTo 用 1 字节存在标志表达(空 = 当前生效);
//   3. 返回序列化字节。
std::vector<std::uint8_t> encodeVersion(const BlockVersion& v) {
    ByteWriter w;
    w.putU32(v.version);
    w.putString(v.payload);
    w.putDouble(v.confidence);
    w.putU8(static_cast<std::uint8_t>(v.state));
    w.putI64(v.validFrom);
    w.putU8(v.validTo.has_value() ? 1 : 0);
    if (v.validTo.has_value()) {
        w.putI64(*v.validTo);
    }
    return w.bytes();
}

// 伪代码:
//   1. 按 encodeVersion 相同的顺序逐字段读出。
void decodeVersion(ByteReader& r, BlockVersion& v) {
    v.version = r.getU32();
    v.payload = r.getString();
    v.confidence = r.getDouble();
    v.state = static_cast<BlockState>(r.getU8());
    v.validFrom = r.getI64();
    if (r.getU8() != 0) {
        v.validTo = r.getI64();
    }
}

// 伪代码:
//   1. 按记录类型分支序列化 payload:PutBlock 写完整块,Confirm 写 id+now,
//      ReportChange 写 id+新负载+新置信度+now,Remove 写 id,
//      ExpireBefore 写时间阈值;
//   2. 返回 payload 字节(帧外壳由 makeFrame 统一添加)。
std::vector<std::uint8_t> encodeWalPayload(const WalRecord& rec) {
    ByteWriter w;
    // 统一前缀:分片键存在标志 + (可选)4 个 i64 分片坐标
    w.putU8(rec.hasShardKey ? 1 : 0);
    if (rec.hasShardKey) {
        w.putI64(rec.shard.sx);
        w.putI64(rec.shard.sy);
        w.putI64(rec.shard.sz);
        w.putI64(rec.shard.tBucket);
        w.putI64(rec.shard.level);
    }
    switch (rec.type) {
        case RecordType::PutBlock:
            w.putBytes(encodeBlock(rec.block));
            break;
        case RecordType::Confirm:
            w.putU64(rec.id);
            w.putI64(rec.ts);
            w.putDouble(rec.weight);  // v5:确认权重
            break;
        case RecordType::ReportChange:
            w.putU64(rec.id);
            w.putString(rec.payload);
            w.putDouble(rec.confidence);
            w.putI64(rec.ts);
            break;
        case RecordType::Observe:
            w.putU64(rec.id);
            w.putString(rec.payload);
            w.putDouble(rec.confidence);
            w.putU64(rec.source);
            w.putI64(rec.ts);
            break;
        case RecordType::RegisterSource:
            w.putU64(rec.source);     // v6:来源 id
            w.putDouble(rec.weight);  // v6:可靠性权重(复用 weight 字段)
            break;
        case RecordType::Remove:
            w.putU64(rec.id);
            break;
        case RecordType::ExpireBefore:
            w.putI64(rec.ts);
            break;
        default:
            break;
    }
    return w.bytes();
}

// 伪代码:
//   1. 按记录类型分支从 payload 解析出 WalRecord 字段;
//   2. 解析完成后检查 reader.ok() 与无剩余字节,失败返回 false。
bool decodeWalPayload(RecordType type, const std::vector<std::uint8_t>& payload,
                      WalRecord& rec) {
    ByteReader r(payload.data(), payload.size());
    rec.type = type;
    // 统一前缀:分片键存在标志 + (可选)4 个 i64 分片坐标
    rec.hasShardKey = (r.getU8() != 0);
    if (rec.hasShardKey) {
        rec.shard.sx = r.getI64();
        rec.shard.sy = r.getI64();
        rec.shard.sz = r.getI64();
        rec.shard.tBucket = r.getI64();
        rec.shard.level = r.getI64();
    }
    switch (type) {
        case RecordType::PutBlock:
            decodeBlock(r, rec.block);
            break;
        case RecordType::Confirm:
            rec.id = r.getU64();
            rec.ts = r.getI64();
            rec.weight = r.getDouble();  // v5:确认权重
            break;
        case RecordType::ReportChange:
            rec.id = r.getU64();
            rec.payload = r.getString();
            rec.confidence = r.getDouble();
            rec.ts = r.getI64();
            break;
        case RecordType::Observe:
            rec.id = r.getU64();
            rec.payload = r.getString();
            rec.confidence = r.getDouble();
            rec.source = r.getU64();
            rec.ts = r.getI64();
            break;
        case RecordType::RegisterSource:
            rec.source = r.getU64();   // v6:来源 id
            rec.weight = r.getDouble();  // v6:可靠性权重
            break;
        case RecordType::Remove:
            rec.id = r.getU64();
            break;
        case RecordType::ExpireBefore:
            rec.ts = r.getI64();
            break;
        default:
            return false;
    }
    return r.ok() && r.remaining() == 0;
}

// ---------------------------------------------------------------------------
// 记录帧与文件级编解码
// ---------------------------------------------------------------------------

// 伪代码:
//   1. 拼装 type + payloadLen + payload;
//   2. 对这三部分计算 crc32 追加在末尾;
//   3. 返回完整帧字节。
std::vector<std::uint8_t> makeFrame(RecordType type,
                                    const std::vector<std::uint8_t>& payload) {
    ByteWriter w;
    w.putU8(static_cast<std::uint8_t>(type));
    w.putU32(static_cast<std::uint32_t>(payload.size()));
    w.putBytes(payload);
    const std::uint32_t crc = crc32(w.bytes().data(), w.bytes().size());
    w.putU32(crc);
    return w.bytes();
}

// 帧解析结果:headerOk 表示 magic/version 通过;clean 表示完整读到末尾;
// frames 为已验证的记录(类型 + payload)列表
struct ParsedFile {
    bool headerOk = false;
    bool clean = false;
    std::vector<std::pair<RecordType, std::vector<std::uint8_t>>> frames;
};

// 伪代码:
//   1. 文件小于文件头:headerOk=false 直接返回;
//   2. 校验 magic 与 formatVersion,不符 headerOk=false 返回;
//   3. 循环解析记录帧:剩余不足帧头+CRC -> 停止(截断尾部);
//      CRC 校验失败 -> 停止(损坏记录);否则收进 frames 继续;
//   4. 恰好读到末尾则 clean=true。
ParsedFile parseFrames(const std::vector<std::uint8_t>& bytes) {
    ParsedFile result;
    if (bytes.size() < kHeaderSize) {
        return result;
    }
    ByteReader r(bytes.data(), bytes.size());
    const std::uint32_t magic = r.getU32();
    const std::uint32_t version = r.getU32();
    if (magic != kMagic || version != kFormatVersion) {
        return result;
    }
    result.headerOk = true;

    while (r.remaining() >= kFrameHeaderSize + kCrcSize) {
        const std::size_t frameStart = bytes.size() - r.remaining();
        const RecordType type = static_cast<RecordType>(r.getU8());
        const std::uint32_t len = r.getU32();
        if (r.remaining() < len + kCrcSize) {
            break;  // 截断的尾部记录
        }
        const std::uint32_t expectedCrc = crc32(bytes.data() + frameStart,
                                                kFrameHeaderSize + len);
        std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(frameStart + kFrameHeaderSize),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(frameStart + kFrameHeaderSize + len));
        r.skip(len);  // 推进读取位置越过 payload,再读 CRC
        const std::uint32_t actualCrc = r.getU32();
        if (expectedCrc != actualCrc) {
            break;  // CRC 不匹配:丢弃该记录及其后内容
        }
        result.frames.emplace_back(type, std::move(payload));
    }
    result.clean = (r.remaining() == 0);
    return result;
}

// 伪代码:
//   1. 写入 magic 与 formatVersion 共 8 字节;
//   2. 返回文件头字节。
std::vector<std::uint8_t> makeHeader() {
    ByteWriter w;
    w.putU32(kMagic);
    w.putU32(kFormatVersion);
    return w.bytes();
}

// 伪代码:
//   1. 打开文件(不存在返回空 optional,由调用方区分「不存在」与「损坏」);
//   2. 读入全部字节返回。
std::optional<std::vector<std::uint8_t>> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// 伪代码:
//   1. 把全部字节写入 路径.tmp 临时文件并关闭;
//   2. rename 为正式文件名(原子替换);
//   3. 任一步失败返回 false。
bool writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// DynamicInstance 编解码(v4)
// ---------------------------------------------------------------------------

// 伪代码:
//   1. 按固定顺序写入实例全部字段:id/类别/包围盒/最新轨迹点/轨迹(计数+
//      逐点)/来源集合(计数+逐个)/置信度/状态/lastSeen/stationarySince;
//   2. 返回序列化字节。
std::vector<std::uint8_t> encodeInstance(const DynamicInstance& inst) {
    ByteWriter w;
    w.putU64(inst.id);
    w.putString(inst.classLabel);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(inst.bounds.min[axis]);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(inst.bounds.max[axis]);
    }
    w.putI64(inst.latest.t);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(inst.latest.position[axis]);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        w.putDouble(inst.latest.velocity[axis]);
    }
    w.putU32(static_cast<std::uint32_t>(inst.trajectory.size()));
    for (const TrackPoint& p : inst.trajectory) {
        w.putI64(p.t);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            w.putDouble(p.position[axis]);
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            w.putDouble(p.velocity[axis]);
        }
    }
    w.putU32(static_cast<std::uint32_t>(inst.sources.size()));
    for (SourceId src : inst.sources) {
        w.putU64(src);
    }
    w.putDouble(inst.confidence);
    w.putU8(static_cast<std::uint8_t>(inst.state));
    w.putI64(inst.lastSeen);
    w.putI64(inst.stationarySince);
    return w.bytes();
}

// 伪代码:
//   1. 按 encodeInstance 相同的顺序逐字段读出;
//   2. 由调用方检查 reader.ok() 判定是否截断。
void decodeInstance(ByteReader& r, DynamicInstance& inst) {
    inst.id = r.getU64();
    inst.classLabel = r.getString();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        inst.bounds.min[axis] = r.getDouble();
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        inst.bounds.max[axis] = r.getDouble();
    }
    inst.latest.t = r.getI64();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        inst.latest.position[axis] = r.getDouble();
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        inst.latest.velocity[axis] = r.getDouble();
    }
    const std::uint32_t pointCount = r.getU32();
    for (std::uint32_t i = 0; i < pointCount && r.ok(); ++i) {
        TrackPoint p;
        p.t = r.getI64();
        for (std::size_t axis = 0; axis < 3; ++axis) {
            p.position[axis] = r.getDouble();
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            p.velocity[axis] = r.getDouble();
        }
        inst.trajectory.push_back(p);
    }
    const std::uint32_t sourceCount = r.getU32();
    for (std::uint32_t i = 0; i < sourceCount && r.ok(); ++i) {
        inst.sources.insert(r.getU64());
    }
    inst.confidence = r.getDouble();
    inst.state = static_cast<InstanceState>(r.getU8());
    inst.lastSeen = r.getI64();
    inst.stationarySince = r.getI64();
}

}  // namespace

// 伪代码:
//   1. 自动创建数据目录;
//   2. 拼装文件头 + 全部 SnapBlock 帧 + 全部 SnapVersion 帧 + SnapshotEnd 帧
//      (SnapshotEnd 的 payload 携带 nextId);
//   3. 原子写入 snapshot.stmb,返回成败。
bool PersistenceManager::saveSnapshot(
    const std::string& dir,
    const std::vector<MemoryBlock>& blocks,
    const std::vector<std::pair<BlockId, BlockVersion>>& versions,
    BlockId nextId,
    const std::vector<DynamicInstance>& instances,
    InstanceId instanceNextId,
    const std::vector<std::pair<SourceId, double>>& sources) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }

    ByteWriter w;
    w.putBytes(makeHeader());
    for (const MemoryBlock& b : blocks) {
        w.putBytes(makeFrame(RecordType::SnapBlock, encodeBlock(b)));
    }
    for (const auto& [id, v] : versions) {
        ByteWriter payload;
        payload.putU64(id);
        payload.putBytes(encodeVersion(v));
        w.putBytes(makeFrame(RecordType::SnapVersion, payload.bytes()));
    }
    // v4:动态层实例随快照落盘(含已归档实例的轨迹,供回溯)
    for (const DynamicInstance& inst : instances) {
        w.putBytes(makeFrame(RecordType::SnapInstance, encodeInstance(inst)));
    }
    // v6:来源注册表小节(source -> reliability)
    for (const auto& [src, reliability] : sources) {
        ByteWriter payload;
        payload.putU64(src);
        payload.putDouble(reliability);
        w.putBytes(makeFrame(RecordType::SnapSource, payload.bytes()));
    }
    ByteWriter endPayload;
    endPayload.putU64(nextId);
    endPayload.putU64(instanceNextId);
    w.putBytes(makeFrame(RecordType::SnapshotEnd, endPayload.bytes()));

    return writeFileAtomic(dir + "/snapshot.stmb", w.bytes());
}

// 伪代码:
//   1. 读取 snapshot.stmb:不存在返回空 SnapshotData(首次启动,非错误);
//   2. 解析帧:header 错误或不完整(截断/CRC 错)返回空 optional(损坏);
//   3. 逐帧解码:SnapBlock 进 blocks,SnapVersion 进 versions,
//      SnapshotEnd 取出 nextId;未知类型视为损坏;
//   4. 缺少 SnapshotEnd(未完整收尾)同样视为损坏。
std::optional<SnapshotData> PersistenceManager::loadSnapshot(const std::string& dir) {
    const std::optional<std::vector<std::uint8_t>> bytes =
        readFile(dir + "/snapshot.stmb");
    if (!bytes.has_value()) {
        return SnapshotData{};  // 文件不存在:空数据而非错误
    }
    const ParsedFile parsed = parseFrames(*bytes);
    if (!parsed.headerOk || !parsed.clean) {
        return std::nullopt;  // magic/version 错误或内容损坏
    }

    SnapshotData data;
    bool sawEnd = false;
    for (const auto& [type, payload] : parsed.frames) {
        ByteReader r(payload.data(), payload.size());
        if (type == RecordType::SnapBlock) {
            MemoryBlock b;
            decodeBlock(r, b);
            if (!r.ok()) {
                return std::nullopt;
            }
            data.blocks.push_back(std::move(b));
        } else if (type == RecordType::SnapVersion) {
            const BlockId id = r.getU64();
            BlockVersion v;
            decodeVersion(r, v);
            if (!r.ok()) {
                return std::nullopt;
            }
            data.versions.emplace_back(id, std::move(v));
        } else if (type == RecordType::SnapInstance) {
            DynamicInstance inst;
            decodeInstance(r, inst);
            if (!r.ok()) {
                return std::nullopt;
            }
            data.instances.push_back(std::move(inst));
        } else if (type == RecordType::SnapSource) {
            const SourceId src = r.getU64();
            const double reliability = r.getDouble();
            if (!r.ok()) {
                return std::nullopt;
            }
            data.sources.emplace_back(src, reliability);
        } else if (type == RecordType::SnapshotEnd) {
            data.nextId = r.getU64();
            data.instanceNextId = r.getU64();
            if (!r.ok()) {
                return std::nullopt;
            }
            sawEnd = true;
        } else {
            return std::nullopt;  // 未知记录类型
        }
    }
    if (!sawEnd) {
        return std::nullopt;
    }
    return data;
}

// 伪代码:
//   1. 自动创建数据目录;
//   2. 若 wal.log 不存在或为空,先写入文件头;
//   3. 把记录序列化为帧追加到文件末尾,返回成败。
bool PersistenceManager::appendWal(const std::string& dir, const WalRecord& record) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    const std::string path = dir + "/wal.log";
    const bool needHeader = !std::filesystem::exists(path) ||
                            std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        return false;
    }
    if (needHeader) {
        const std::vector<std::uint8_t> header = makeHeader();
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
    }
    const std::vector<std::uint8_t> frame =
        makeFrame(record.type, encodeWalPayload(record));
    out.write(reinterpret_cast<const char*>(frame.data()),
              static_cast<std::streamsize>(frame.size()));
    return static_cast<bool>(out);
}

// 伪代码:
//   1. 读取 wal.log:不存在返回空记录列表(首次启动,非错误);
//   2. 解析帧:header 错误(magic/version)返回空 optional(报错);
//   3. 逐帧解码为 WalRecord:解码失败视为损坏记录,停止并保留已验证前缀;
//   4. 返回记录列表(clean 与否均可接受,截断尾部已被解析器丢弃)。
std::optional<std::vector<WalRecord>> PersistenceManager::replayWal(
    const std::string& dir) {
    const std::optional<std::vector<std::uint8_t>> bytes = readFile(dir + "/wal.log");
    if (!bytes.has_value()) {
        return std::vector<WalRecord>{};  // 文件不存在:空列表而非错误
    }
    const ParsedFile parsed = parseFrames(*bytes);
    if (!parsed.headerOk) {
        return std::nullopt;  // magic/version 错误:报错
    }

    std::vector<WalRecord> records;
    for (const auto& [type, payload] : parsed.frames) {
        WalRecord rec;
        if (!decodeWalPayload(type, payload, rec)) {
            break;  // 损坏记录:保留已验证前缀
        }
        records.push_back(std::move(rec));
    }
    return records;
}

// 伪代码:
//   1. 调用 saveSnapshot 落全量快照,失败返回 false;
//   2. 用只含文件头的空内容覆盖 wal.log(截断日志);
//   3. 返回成败。
bool PersistenceManager::checkpoint(
    const std::string& dir,
    const std::vector<MemoryBlock>& blocks,
    const std::vector<std::pair<BlockId, BlockVersion>>& versions,
    BlockId nextId,
    const std::vector<DynamicInstance>& instances,
    InstanceId instanceNextId,
    const std::vector<std::pair<SourceId, double>>& sources) {
    if (!saveSnapshot(dir, blocks, versions, nextId, instances, instanceNextId,
                      sources)) {
        return false;
    }
    return writeFileAtomic(dir + "/wal.log", makeHeader());
}

// 伪代码:
//   1. 把 ShardKey 的五个分量用下划线连接,加 .stmb 后缀返回
//      (level 在前,同层级的分片文件按字典序自然相邻)。
std::string PersistenceManager::shardFileName(const ShardKey& key) {
    return std::to_string(key.level) + "_" + std::to_string(key.sx) + "_" +
           std::to_string(key.sy) + "_" + std::to_string(key.sz) + "_" +
           std::to_string(key.tBucket) + ".stmb";
}

// 伪代码:
//   1. 自动创建 dir/shards 子目录;
//   2. 拼装文件头 + 全部 SnapBlock 帧 + 全部 SnapVersion 帧 + SnapshotEnd 帧
//      (空载荷,仅作结束标记);
//   3. 原子写入分片文件,返回成败。
bool PersistenceManager::saveShardFile(const std::string& dir, const ShardKey& key,
                                       const ShardData& data) {
    const std::string shardDir = dir + "/shards";
    std::error_code ec;
    std::filesystem::create_directories(shardDir, ec);
    if (ec) {
        return false;
    }
    ByteWriter w;
    w.putBytes(makeHeader());
    for (const MemoryBlock& b : data.blocks) {
        w.putBytes(makeFrame(RecordType::SnapBlock, encodeBlock(b)));
    }
    for (const auto& [id, v] : data.versions) {
        ByteWriter payload;
        payload.putU64(id);
        payload.putBytes(encodeVersion(v));
        w.putBytes(makeFrame(RecordType::SnapVersion, payload.bytes()));
    }
    w.putBytes(makeFrame(RecordType::SnapshotEnd, {}));
    return writeFileAtomic(shardDir + "/" + shardFileName(key), w.bytes());
}

// 伪代码:
//   1. 读取分片文件:不存在返回空 ShardData(空分片,非错误);
//   2. 解析帧:header 错误或不完整返回空 optional(损坏);
//   3. 逐帧解码 SnapBlock / SnapVersion,见到 SnapshotEnd 收尾;
//   4. 缺少结束标记或未知类型视为损坏。
std::optional<ShardData> PersistenceManager::loadShardFile(const std::string& dir,
                                                           const ShardKey& key) {
    const std::optional<std::vector<std::uint8_t>> bytes =
        readFile(dir + "/shards/" + shardFileName(key));
    if (!bytes.has_value()) {
        return ShardData{};  // 不存在 = 空分片
    }
    const ParsedFile parsed = parseFrames(*bytes);
    if (!parsed.headerOk || !parsed.clean) {
        return std::nullopt;
    }
    ShardData data;
    bool sawEnd = false;
    for (const auto& [type, payload] : parsed.frames) {
        ByteReader r(payload.data(), payload.size());
        if (type == RecordType::SnapBlock) {
            MemoryBlock b;
            decodeBlock(r, b);
            if (!r.ok()) {
                return std::nullopt;
            }
            data.blocks.push_back(std::move(b));
        } else if (type == RecordType::SnapVersion) {
            const BlockId id = r.getU64();
            BlockVersion v;
            decodeVersion(r, v);
            if (!r.ok()) {
                return std::nullopt;
            }
            data.versions.emplace_back(id, std::move(v));
        } else if (type == RecordType::SnapshotEnd) {
            sawEnd = true;
        } else {
            return std::nullopt;
        }
    }
    if (!sawEnd) {
        return std::nullopt;
    }
    return data;
}

// 伪代码:
//   1. 自动创建数据目录;
//   2. 拼装文件头 + ManifestMeta 帧(全局参数 + nextId + maxBlockRadius)
//      + 每个分片一条 ManifestShard 帧 + SnapshotEnd 帧;
//   3. 原子写入 manifest.stmb,返回成败。
bool PersistenceManager::saveManifest(const std::string& dir,
                                      const ManifestData& manifest) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    ByteWriter w;
    w.putBytes(makeHeader());
    ByteWriter meta;
    meta.putU64(manifest.capacity);
    meta.putDouble(manifest.cellSize);
    meta.putI64(manifest.timeSlotMs);
    meta.putI64(manifest.confirmThreshold);
    meta.putI64(manifest.decayHalfLifeMs);
    meta.putDouble(manifest.shardCellSize);
    meta.putI64(manifest.shardTimeSpanMs);
    meta.putU64(manifest.maxLoadedShards);
    meta.putU64(manifest.nextId);
    meta.putDouble(manifest.maxBlockRadius);
    meta.putU64(manifest.levelCount);
    w.putBytes(makeFrame(RecordType::ManifestMeta, meta.bytes()));
    for (const ManifestShardEntry& e : manifest.shards) {
        ByteWriter payload;
        payload.putI64(e.key.sx);
        payload.putI64(e.key.sy);
        payload.putI64(e.key.sz);
        payload.putI64(e.key.tBucket);
        payload.putI64(e.key.level);
        payload.putU32(static_cast<std::uint32_t>(e.blockIds.size()));
        payload.putU32(e.versionCount);
        for (BlockId id : e.blockIds) {
            payload.putU64(id);
        }
        w.putBytes(makeFrame(RecordType::ManifestShard, payload.bytes()));
    }
    // v6:来源注册表小节(source -> reliability)
    for (const auto& [src, reliability] : manifest.sources) {
        ByteWriter payload;
        payload.putU64(src);
        payload.putDouble(reliability);
        w.putBytes(makeFrame(RecordType::SnapSource, payload.bytes()));
    }
    w.putBytes(makeFrame(RecordType::SnapshotEnd, {}));
    return writeFileAtomic(dir + "/manifest.stmb", w.bytes());
}

// 伪代码:
//   1. 读取 manifest.stmb:不存在返回 present=false 的默认值(首次启动);
//   2. 解析帧:header 错误或不完整返回空 optional(损坏);
//   3. ManifestMeta 帧解出全局参数,ManifestShard 帧逐条解出分片清单;
//   4. 缺少 ManifestMeta 或 SnapshotEnd 视为损坏;成功则 present=true。
std::optional<ManifestData> PersistenceManager::loadManifest(const std::string& dir) {
    const std::optional<std::vector<std::uint8_t>> bytes =
        readFile(dir + "/manifest.stmb");
    if (!bytes.has_value()) {
        return ManifestData{};  // 不存在:present=false
    }
    const ParsedFile parsed = parseFrames(*bytes);
    if (!parsed.headerOk || !parsed.clean) {
        return std::nullopt;
    }
    ManifestData data;
    bool sawMeta = false;
    bool sawEnd = false;
    for (const auto& [type, payload] : parsed.frames) {
        ByteReader r(payload.data(), payload.size());
        if (type == RecordType::ManifestMeta) {
            data.capacity = r.getU64();
            data.cellSize = r.getDouble();
            data.timeSlotMs = r.getI64();
            data.confirmThreshold = r.getI64();
            data.decayHalfLifeMs = r.getI64();
            data.shardCellSize = r.getDouble();
            data.shardTimeSpanMs = r.getI64();
            data.maxLoadedShards = r.getU64();
            data.nextId = r.getU64();
            data.maxBlockRadius = r.getDouble();
            data.levelCount = r.getU64();
            if (!r.ok()) {
                return std::nullopt;
            }
            sawMeta = true;
        } else if (type == RecordType::ManifestShard) {
            ManifestShardEntry e;
            e.key.sx = r.getI64();
            e.key.sy = r.getI64();
            e.key.sz = r.getI64();
            e.key.tBucket = r.getI64();
            e.key.level = r.getI64();
            const std::uint32_t count = r.getU32();
            e.versionCount = r.getU32();
            for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
                e.blockIds.push_back(r.getU64());
            }
            if (!r.ok() || e.blockIds.size() != count) {
                return std::nullopt;
            }
            data.shards.push_back(std::move(e));
        } else if (type == RecordType::SnapSource) {
            const SourceId src = r.getU64();
            const double reliability = r.getDouble();
            if (!r.ok()) {
                return std::nullopt;
            }
            data.sources.emplace_back(src, reliability);
        } else if (type == RecordType::SnapshotEnd) {
            sawEnd = true;
        } else {
            return std::nullopt;
        }
    }
    if (!sawMeta || !sawEnd) {
        return std::nullopt;
    }
    data.present = true;
    return data;
}

// 伪代码:
//   1. 自动创建数据目录;
//   2. 用只含文件头的空内容覆盖 wal.log,返回成败。
bool PersistenceManager::truncateWal(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    return writeFileAtomic(dir + "/wal.log", makeHeader());
}

// 伪代码:
//   1. 自动创建数据目录;
//   2. 拼装文件头 + 全部 SnapInstance 帧 + SnapshotEnd 帧(携带实例 id
//      发生器状态);
//   3. 原子写入 dynamic.stmb,返回成败。
bool PersistenceManager::saveDynamic(
    const std::string& dir,
    const std::vector<DynamicInstance>& instances,
    InstanceId instanceNextId) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    ByteWriter w;
    w.putBytes(makeHeader());
    for (const DynamicInstance& inst : instances) {
        w.putBytes(makeFrame(RecordType::SnapInstance, encodeInstance(inst)));
    }
    ByteWriter endPayload;
    endPayload.putU64(instanceNextId);
    w.putBytes(makeFrame(RecordType::SnapshotEnd, endPayload.bytes()));
    return writeFileAtomic(dir + "/dynamic.stmb", w.bytes());
}

// 伪代码:
//   1. 读取 dynamic.stmb:不存在返回空数据(首次启动,非错误);
//   2. 解析帧:header 错误或不完整返回空 optional(损坏);
//   3. SnapInstance 帧逐个解码进实例列表,SnapshotEnd 取出实例 id 发生器;
//   4. 缺少结束标记或未知类型视为损坏。
std::optional<std::pair<std::vector<DynamicInstance>, InstanceId>>
PersistenceManager::loadDynamic(const std::string& dir) {
    const std::optional<std::vector<std::uint8_t>> bytes =
        readFile(dir + "/dynamic.stmb");
    if (!bytes.has_value()) {
        return std::make_pair(std::vector<DynamicInstance>{}, InstanceId{1});
    }
    const ParsedFile parsed = parseFrames(*bytes);
    if (!parsed.headerOk || !parsed.clean) {
        return std::nullopt;
    }
    std::vector<DynamicInstance> instances;
    InstanceId nextId = 1;
    bool sawEnd = false;
    for (const auto& [type, payload] : parsed.frames) {
        ByteReader r(payload.data(), payload.size());
        if (type == RecordType::SnapInstance) {
            DynamicInstance inst;
            decodeInstance(r, inst);
            if (!r.ok()) {
                return std::nullopt;
            }
            instances.push_back(std::move(inst));
        } else if (type == RecordType::SnapshotEnd) {
            nextId = r.getU64();
            if (!r.ok()) {
                return std::nullopt;
            }
            sawEnd = true;
        } else {
            return std::nullopt;
        }
    }
    if (!sawEnd) {
        return std::nullopt;
    }
    return std::make_pair(std::move(instances), nextId);
}

}  // namespace stmb

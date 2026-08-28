// ============================================================================
// anchor_path.hpp —— 路径码 AnchorPath：锚点身份的纯函数化（规范 §3.1）
// ============================================================================
//
// 【本文件的实现思路】
// 设计原则 2："身份是函数，不是记录"。锚点无限细分 ⇒ 不能被存储，只能被
// 计算。路径码是从全局根到锚点的逐段子区编号序列（论文定义 5.1）：
//
//     P(A) = [c0, c1, ..., c_{d-1}]，ci 为第 i 层子区在父区内的编号
//
// 段编号分两个命名空间（规范 §3.1 编码建议）：
//   - 匿名细分段：欧氏场景为 Morton 位组 0..7（x|y|z 各 1 bit，base=2 时
//     每层恰好 3 bit，2 的幂在二进制浮点中精确表示，见论文 §5.2）；
//     球面/轨道场景为 0..3（经纬/径角各 1 bit），一维场景为 0..1；
//   - 语义命名段：由 Kernel 分配的 ≥ kSemanticCodeBase 的独立编号，
//     与规则细分永不冲突。
//
// 关键取舍：
//   - 定长数组 + depth（MAX_DEPTH=32），避免堆分配（规范 §3.1 建议）；
//   - HashKey() 优先走"整条路径打包进单个 uint64"的快路径：深度 ≤ 18 且
//     全部段为 3 bit Morton 码时，低位 54 bit 装段码、高位 8 bit 装深度，
//     哈希与相等比较退化为整数运算；否则退化为 FNV-1a（语义段路径），
//     并置最高位以与打包形式隔离，保证两种编码永不碰撞。
//   - 哈希表以完整 AnchorPath 为键（hash + 全等比较），HashKey 相同但
//     路径不同的情况由 unordered_map 的相等比较正确处理，不依赖
//     "哈希唯一"这种脆弱假设。
//
// 四个基本运算（论文 §5.2）均为常数或线性时间：
//   Parent()      —— 去末段，O(1) 拷贝；
//   Prefix(d)     —— 前 d 段，O(d) 拷贝；
//   IsPrefixOf()  —— 包含关系 = 前缀匹配（LCA、OwnerOf 的基础）；
//   LcaDepth()    —— 最长公共前缀长度（算法 S3/S5 的行走骨架）。
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace geocore {

// 最大树深：定长数组避免堆分配；32 层 × base=2 细分，配合场景自身的
// 尺度，足以覆盖 10^11 m 到 10^-3 m 的动态范围。
inline constexpr int kMaxDepth = 32;

// 语义命名段的最小编号：规则细分段恒 < 8（欧氏 0..7，其余模型更少），
// 语义段从 256 起编号，留出充足间隔防止未来细分扇出扩展。
inline constexpr uint32_t kSemanticCodeBase = 256;

// ----------------------------------------------------------------------------
// AnchorPath：变长层级路径（定长存储）
// ----------------------------------------------------------------------------
struct AnchorPath {
    std::array<uint32_t, kMaxDepth> codes{};  // codes[i] = 第 i 层子区编号
    uint8_t depth = 0;                        // 段数；depth==0 表示全局根

    // 父路径：去掉末段（O(1) 视图式拷贝）。算法 S3 上行行走的基本步。
    AnchorPath Parent() const {
        AnchorPath r = *this;
        if (r.depth > 0) --r.depth;
        return r;
    }

    // 前 d 段：算法 S3 下行行走按 to.Prefix(d) 逐层下探。
    AnchorPath Prefix(int d) const {
        AnchorPath r = *this;
        r.depth = static_cast<uint8_t>(d < 0 ? 0 : (d > kMaxDepth ? kMaxDepth : d));
        return r;
    }

    // 追加一段，返回新路径（函数式风格，便于表达式组合）
    AnchorPath Append(uint32_t c) const {
        AnchorPath r = *this;
        r.AppendInPlace(c);
        return r;
    }

    // 原地追加一段：AnchorOf（算法 S1）逐层量化下沉的热路径使用
    void AppendInPlace(uint32_t c) {
        if (depth < kMaxDepth) codes[depth++] = c;
    }

    // 末段编号：TransformAt 虚拟节点分支（算法 S2）取 seg = path.LastSeg()
    uint32_t LastSeg() const { return depth > 0 ? codes[depth - 1] : 0; }

    // 包含关系：本路径是否为 other 的前缀（祖先判定）
    // 伪码：IsPrefixOf(o) ⟺ depth ≤ o.depth ∧ ∀i<depth: codes[i]==o.codes[i]
    bool IsPrefixOf(const AnchorPath& o) const {
        if (depth > o.depth) return false;
        return std::memcmp(codes.data(), o.codes.data(),
                           depth * sizeof(uint32_t)) == 0;
    }

    // 完整相等：哈希表键比较（不依赖哈希唯一性，正确性兜底）
    bool operator==(const AnchorPath& o) const {
        return depth == o.depth &&
               std::memcmp(codes.data(), o.codes.data(),
                           depth * sizeof(uint32_t)) == 0;
    }
    bool operator!=(const AnchorPath& o) const { return !(*this == o); }

    // 字典序（先逐段比较，再按深度）：OwnerOf 排序 + 二分（规范 §3.4）需要
    bool operator<(const AnchorPath& o) const {
        int n = depth < o.depth ? depth : o.depth;
        for (int i = 0; i < n; ++i) {
            if (codes[i] != o.codes[i]) return codes[i] < o.codes[i];
        }
        return depth < o.depth;
    }

    // ------------------------------------------------------------------
    // HashKey：定长编码的哈希键（深度打包进高位）
    // 伪码（规范 §3.1）：
    //   if depth ≤ 18 ∧ 所有段 < 8:
    //       key = (depth << 56) | Σ codes[i] << (3i)   // 整数打包快路径
    //   else:
    //       key = FNV1a(depth, codes) | (1 << 63)      // 语义段退化路径
    // 18 层的限制来自 3bit×18=54bit 段码 + 8bit 深度 ≤ 64bit。
    // ------------------------------------------------------------------
    uint64_t HashKey() const {
        bool packable = depth <= 18;
        if (packable) {
            for (int i = 0; i < depth; ++i) {
                if (codes[i] >= 8) { packable = false; break; }
            }
        }
        if (packable) {
            // 快路径：整条路径 = 一个整数，哈希与比较退化为整数运算
            uint64_t key = static_cast<uint64_t>(depth) << 56;
            for (int i = 0; i < depth; ++i)
                key |= static_cast<uint64_t>(codes[i]) << (3 * i);
            return key;  // 最高字节 ≤ 18，恒 < 0x80，与 FNV 路径隔离
        }
        // 退化路径：FNV-1a 覆盖深度与全部原始段码
        uint64_t h = 14695981039346656037ull;
        auto mix = [&h](uint64_t v) {
            for (int b = 0; b < 8; ++b) {
                h ^= (v >> (8 * b)) & 0xff;
                h *= 1099511628211ull;
            }
        };
        mix(depth);
        for (int i = 0; i < depth; ++i) mix(codes[i]);
        return h | (1ull << 63);  // 置最高位，与打包快路径永不碰撞
    }
};

// ----------------------------------------------------------------------------
// LcaDepth：两条路径的最近公共祖先深度 = 最长公共前缀长度（论文 §5.2）
// 伪码：lca = max{ d : a.Prefix(d) == b.Prefix(d) }
// 算法 S3 的行走链长 = d_a + d_b − 2·lca（纪律 P5：走 LCA 不绕根）。
// ----------------------------------------------------------------------------
inline int LcaDepth(const AnchorPath& a, const AnchorPath& b) {
    int n = a.depth < b.depth ? a.depth : b.depth;
    int i = 0;
    while (i < n && a.codes[i] == b.codes[i]) ++i;
    return i;
}

// unordered_map / unordered_set 的哈希函子：委托给 HashKey()
struct PathHash {
    size_t operator()(const AnchorPath& p) const {
        return static_cast<size_t>(p.HashKey());
    }
};

// ----------------------------------------------------------------------------
// 字符串序列化 / 解析：路径码的文本形式 = 段码十进制以 '/' 连接
// （"1/2/3"），全局根（depth==0）为空字符串。
// 供上层系统（如存储层把 anchor_ref 存为字符串列）持久化与传输使用；
// 解析失败返回 false，调用方可降级为普通字符串处理。
// ----------------------------------------------------------------------------
inline std::string ToString(const AnchorPath& p) {
    std::string s;
    for (int i = 0; i < p.depth; ++i) {
        if (i > 0) s += '/';
        s += std::to_string(p.codes[i]);
    }
    return s;
}

// 解析 ToString 的文本形式。""/"/" → 全局根；空段、非数字字符、
// 深度超 kMaxDepth、段码超 uint32 均为非法，返回 false（out 不被修改）。
inline bool ParseAnchorPath(const std::string& s, AnchorPath& out) {
    if (s.empty() || s == "/") {
        out = AnchorPath{};
        return true;
    }
    AnchorPath r;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('/', i);
        std::string seg = s.substr(i, j == std::string::npos ? j : j - i);
        if (seg.empty()) return false;               // 空段："1//2"、"/1"、"1/"
        uint32_t code = 0;
        for (char ch : seg) {
            if (ch < '0' || ch > '9') return false;  // 非数字段
            uint64_t v = (uint64_t)code * 10 + (uint64_t)(ch - '0');
            if (v > 0xffffffffull) return false;     // 段码超 uint32
            code = (uint32_t)v;
        }
        if (r.depth >= kMaxDepth) return false;      // 超深
        r.AppendInPlace(code);
        if (j == std::string::npos) break;
        i = j + 1;
        if (i >= s.size()) return false;             // 末尾 '/'："1/"
    }
    out = r;
    return true;
}

} // namespace geocore

// ============================================================================
// 文件: vql.h
// 模块: stmb_vql(VQL 查询语言,依赖 service)
// 用途: 声明 VqlEngine —— VQL(Voxel Query Language)的解析与执行。
// 语法(关键字大小写不敏感):
//   FIND BLOCKS IN REGION(x1,y1,z1,x2,y2,z2) DURING(t1,t2)
//       [LEVEL n] [STATE Pending|Stable|Changing] [PAYLOAD ~ '子串']
//       [AT t] [SUMMARY] [LIMIT n]
//   FIND HISTORY OF <blockId>
//   FIND DYNAMIC IN REGION(...) DURING(t1,t2)
//   FIND TRAJECTORY OF <instanceId>
//   STATS
// 语义:
//   AT t      : 时间回溯,命中块改查 t 时刻生效的版本内容(getAt);
//   SUMMARY   : 走 queryCoarseToFine 先粗后细;
//   LIMIT n   : 截断结果数量;
//   STATE/PAYLOAD ~ : 对命中结果做后过滤。
// 错误:解析错误给出「位置 N: 期望 xxx,得到 yyy」;语义错误(如 STATE 非法值)
//       单独归类,均不崩溃。
// 架构角色: vql 模块面向文本查询的出口,与 Function Calling 并列。
// ============================================================================
#pragma once

#include "json.h"
#include "stmb_service.h"

#include <string>

namespace stmb {

// VQL 执行结果:ok=false 时 error 为位置化错误信息
struct VqlResult {
    bool ok = false;
    std::string error;
    JsonValue data;  // 语句结果(数组或对象)
};

class VqlEngine {
public:
    // 构造:持有服务引用(语句直接作用于该服务)
    explicit VqlEngine(StmbService& service) : service_(service) {}

    // 解析并执行一条 VQL 语句,返回结构化结果
    VqlResult execute(const std::string& text);

private:
    StmbService& service_;
};

}  // namespace stmb

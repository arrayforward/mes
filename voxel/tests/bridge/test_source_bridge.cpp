// ============================================================================
// 文件: tests/bridge/test_source_bridge.cpp
// 模块: stmb_tests(UnistoreSourceBridge 单元测试,仅 STMB_WITH_UNISTORE=ON)
// 覆盖范围:
//   1. stmb -> 表:registerSource 后经 entitytree::EntityStore.get_source
//      可见,updated_at 被刷新,既有 meta 保留;
//   2. 表 -> stmb:外部(模拟 entity 组件)upsert_source 改权重,新服务 +
//      新桥水合后 reliabilityOf 返回表中新值(冲突以表为准);
//   3. 语义差异:非数值 source_id 水合跳过;未知来源仍默认 0.5;
//   4. 仲裁联测:水合后的权重直接参与加权确认(阈值跨越行为一致)。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "entitytree/backends/sql_store.h"
#include "stmb_service.h"
#include "unistore_source_bridge.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using namespace stmb;
namespace fs = std::filesystem;

// 伪代码:临时目录下构造 tests 专属子目录,先清残留再重建并返回路径。
std::string freshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "stmb_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

MemoryBlock makeBlock(double cx, TimeStamp ts, const std::string& payload) {
    MemoryBlock b;
    b.region.min = {cx - 2.0, -2.0, -2.0};
    b.region.max = {cx + 2.0, 2.0, 2.0};
    b.payload = payload;
    b.timestamp = ts;
    return b;
}

}  // namespace

int main() {
    const std::string dir = freshDir("source_bridge");
    const std::string db = dir + "/world.db";

    // ---- 1. stmb -> 表:registerSource 变更同步到 et_sources ----
    {
        StmbService service(10, 1.0, 1);
        entitytree::SqlEntityStore store(db);
        UnistoreSourceBridge bridge(service, store);   // 空表水合 + 挂钩

        service.registerSource(7, 0.9);
        const auto got = store.get_source("7");
        assert(got.has_value());
        assert(std::abs(got->reliability - 0.9) < 1e-9);
        assert(got->updated_at > 0);

        // 变更再次同步;且表中预置的 meta 被保留
        store.upsert_source(entitytree::SourceRecord{
            "7", 0.9, got->updated_at, {{"vendor", "stmb"}}});
        service.registerSource(7, 0.85);
        const auto got2 = store.get_source("7");
        assert(std::abs(got2->reliability - 0.85) < 1e-9);
        assert(got2->meta.value("vendor", "") == "stmb");

        // 外部(模拟 entity 组件反馈闭环)直接写表
        store.upsert_source(entitytree::SourceRecord{"8", 0.33, 0, {}});
        // 非数值 id:表中可存在,水合时跳过(不映射到 u64 SourceId)
        store.upsert_source(entitytree::SourceRecord{"cam-x", 0.1, 0, {}});
    }

    // ---- 2. 表 -> stmb:重启水合,冲突以表为准 ----
    {
        StmbService service2(10, 1.0, 1);
        entitytree::SqlEntityStore store(db);
        UnistoreSourceBridge bridge2(service2, store);

        assert(std::abs(service2.reliabilityOf(7) - 0.85) < 1e-9);  // 表中值载入
        assert(std::abs(service2.reliabilityOf(8) - 0.33) < 1e-9);  // 外部写入读回
        assert(std::abs(service2.reliabilityOf(99) - 0.5) < 1e-9);  // 未知默认 0.5

        // 仲裁联测:水合权重直接参与加权确认(阈值 1:0.85 一次不到,两次达阈)
        ServiceConfig cfg;
        cfg.capacity = 10;
        cfg.cellSize = 1.0;
        cfg.timeSlotMs = 1;
        cfg.confirmThreshold = 1.0;
        StmbService svc3(cfg);
        UnistoreSourceBridge bridge3(svc3, store);
        svc3.put(makeBlock(5.0, 100, "X"));                    // id1,Pending
        svc3.confirm(1, 7, 200);                               // +0.85 < 1.0
        assert(svc3.get(1)->state == BlockState::Pending);
        svc3.confirm(1, 7, 300);                               // +0.85 = 1.7 >= 1.0
        assert(svc3.get(1)->state == BlockState::Stable);
    }

    fs::remove_all(dir);
    std::cout << "ALL TESTS PASS (source_bridge)\n";
    return 0;
}

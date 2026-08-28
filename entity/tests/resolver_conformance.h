#pragma once

namespace entitytree {
class EntityStore;
}

/// resolver 算法一致性套件：同一套测试跑在所有 EntityStore 后端上。
/// 覆盖：浮现/合并/拆分/检索排序/锚点层级邻近/时效衰减/来源可靠性闭环。
/// 约定：传入一个空的新 store。
void run_resolver_conformance(entitytree::EntityStore& store);

#pragma once

namespace entitytree {
class EntityStore;
}

/// entitytree 后端一致性套件：同一套测试跑在所有 EntityStore 实现上。
/// 约定：传入一个空的新 store。
void run_entity_conformance(entitytree::EntityStore& store);

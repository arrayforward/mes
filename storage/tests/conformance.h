#pragma once

namespace eventstore {
class EventStore;
}

/// 后端一致性套件：同一套测试跑在所有 EventStore 实现上，保证行为一致。
/// 约定：传入一个空的新 store。
void run_conformance_suite(eventstore::EventStore& store);

#pragma once

namespace storage { class RecordBackend; }

/// RecordBackend 级一致性测试：所有后端跑同一套用例（含新算子 Prefix / Ne）。
void run_record_conformance(storage::RecordBackend& backend);

#pragma once

#include <stdexcept>
#include <string>

namespace eventstore {

/// 存储层通用错误（含 SQL 错误、时间线环检测等）。
class EventStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 查询的对象不存在，或追加时引用的父对象不存在。
class NotFoundError : public EventStoreError {
public:
    explicit NotFoundError(const std::string& what)
        : EventStoreError("not found: " + what) {}
};

/// 追加了已存在的对象（主键冲突）。append-only 下不允许重复写入。
class DuplicateError : public EventStoreError {
public:
    explicit DuplicateError(const std::string& what)
        : EventStoreError("duplicate: " + what) {}
};

} // namespace eventstore

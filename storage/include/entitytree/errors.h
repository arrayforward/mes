#pragma once

#include <stdexcept>
#include <string>

namespace entitytree {

/// 存储层通用错误。
class EntityTreeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 查询的对象不存在，或追加时引用的父对象不存在。
class NotFoundError : public EntityTreeError {
public:
    explicit NotFoundError(const std::string& what)
        : EntityTreeError("not found: " + what) {}
};

/// 写入了已存在的对象（主键冲突）。append-only 表不允许重复写入。
class DuplicateError : public EntityTreeError {
public:
    explicit DuplicateError(const std::string& what)
        : EntityTreeError("duplicate: " + what) {}
};

} // namespace entitytree

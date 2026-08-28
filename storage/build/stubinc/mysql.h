/* 仅用于本地语法检查的 stub：声明 mysql_backend.cpp 用到的 libmysqlclient API 子集。
   不进入正式构建（正式构建由 CMake 探测真实头文件）。 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct st_mysql MYSQL;
typedef struct st_mysql_res MYSQL_RES;
typedef char **MYSQL_ROW;
typedef unsigned long long my_ulonglong;
MYSQL *mysql_init(MYSQL *);
MYSQL *mysql_real_connect(MYSQL *, const char *, const char *, const char *,
                          const char *, unsigned int, const char *, unsigned long);
void mysql_close(MYSQL *);
const char *mysql_error(MYSQL *);
unsigned int mysql_errno(MYSQL *);
int mysql_query(MYSQL *, const char *);
unsigned long mysql_real_escape_string(MYSQL *, char *, const char *, unsigned long);
my_ulonglong mysql_insert_id(MYSQL *);
my_ulonglong mysql_affected_rows(MYSQL *);
MYSQL_RES *mysql_store_result(MYSQL *);
MYSQL_ROW mysql_fetch_row(MYSQL_RES *);
void mysql_free_result(MYSQL_RES *);
#ifdef __cplusplus
}
#endif

/* 仅用于本地语法检查的 stub：声明 pg_backend.cpp 用到的 libpq API 子集。
   不进入正式构建（正式构建由 CMake 探测真实头文件）。 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PGconn PGconn;
typedef struct PGresult PGresult;
typedef enum { PGRES_EMPTY_QUERY = 0, PGRES_COMMAND_OK, PGRES_TUPLES_OK } ExecStatusType;
typedef enum { CONNECTION_OK = 0, CONNECTION_BAD } ConnStatusType;
PGconn *PQconnectdb(const char *);
ConnStatusType PQstatus(const PGconn *);
char *PQerrorMessage(const PGconn *);
void PQfinish(PGconn *);
PGresult *PQexec(PGconn *, const char *);
PGresult *PQexecParams(PGconn *, const char *, int, const void *,
                       const char *const *, const int *, const int *, int);
ExecStatusType PQresultStatus(const PGresult *);
char *PQresultErrorMessage(const PGresult *);
void PQclear(PGresult *);
int PQntuples(const PGresult *);
char *PQgetvalue(const PGresult *, int, int);
int PQgetisnull(const PGresult *, int, int);
char *PQcmdTuples(const PGresult *);
#ifdef __cplusplus
}
#endif

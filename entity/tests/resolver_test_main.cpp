#ifdef _WIN32
#include <windows.h>
#endif

#include "entitytree/backends/memory_store.h"
#include "entitytree/backends/sql_store.h"
#include "resolver_conformance.h"
#include "test_framework.h"

using namespace entitytree;

TEST(memory_resolver_conformance) {
    MemoryEntityStore store;
    run_resolver_conformance(store);
}

TEST(sqlite_resolver_conformance) {
    SqlEntityStore store(":memory:");
    run_resolver_conformance(store);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    return tfw::run_all();
}

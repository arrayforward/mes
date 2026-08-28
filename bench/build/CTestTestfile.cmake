# CMake generated Testfile for 
# Source directory: D:/manufacture/bench
# Build directory: D:/manufacture/bench/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[tick_bench_smoke]=] "D:/manufacture/bench/build/tick_bench.exe" "--ticks=200" "--warmup=20" "--backend=memory")
set_tests_properties([=[tick_bench_smoke]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/bench/CMakeLists.txt;56;add_test;D:/manufacture/bench/CMakeLists.txt;0;")
subdirs("knowledge")
subdirs("geocore")

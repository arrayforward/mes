# CMake generated Testfile for 
# Source directory: D:/manufacture/entity
# Build directory: D:/manufacture/entity/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[entity_resolver]=] "D:/manufacture/entity/build/entity_tests.exe")
set_tests_properties([=[entity_resolver]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/entity/CMakeLists.txt;52;add_test;D:/manufacture/entity/CMakeLists.txt;0;")
subdirs("storage")
subdirs("geocore")

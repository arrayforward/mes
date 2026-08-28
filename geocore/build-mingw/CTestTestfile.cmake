# CMake generated Testfile for 
# Source directory: D:/manufacture/geocore
# Build directory: D:/manufacture/geocore/build-mingw
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(unit_tests "D:/manufacture/geocore/build-mingw/unit_tests.exe")
set_tests_properties(unit_tests PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/geocore/CMakeLists.txt;42;add_test;D:/manufacture/geocore/CMakeLists.txt;0;")
add_test(e2e_tests "D:/manufacture/geocore/build-mingw/e2e_tests.exe")
set_tests_properties(e2e_tests PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/geocore/CMakeLists.txt;46;add_test;D:/manufacture/geocore/CMakeLists.txt;0;")

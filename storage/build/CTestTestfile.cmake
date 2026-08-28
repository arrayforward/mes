# CMake generated Testfile for 
# Source directory: D:/manufacture/storage
# Build directory: D:/manufacture/storage/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[eventstore_conformance]=] "D:/manufacture/storage/build/eventstore_tests.exe")
set_tests_properties([=[eventstore_conformance]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/storage/CMakeLists.txt;108;add_test;D:/manufacture/storage/CMakeLists.txt;0;")
add_test([=[voxel_conformance]=] "D:/manufacture/storage/build/voxel_tests.exe")
set_tests_properties([=[voxel_conformance]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/storage/CMakeLists.txt;109;add_test;D:/manufacture/storage/CMakeLists.txt;0;")
add_test([=[entity_conformance]=] "D:/manufacture/storage/build/entity_tests.exe")
set_tests_properties([=[entity_conformance]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/manufacture/storage/CMakeLists.txt;110;add_test;D:/manufacture/storage/CMakeLists.txt;0;")

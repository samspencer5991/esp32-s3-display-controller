# Portable (no ESP-IDF dependencies) - also built by sim/CMakeLists.txt.
idf_component_register(SRCS "protocol.cpp" "controller.cpp" "sim_control_box.cpp"
                       INCLUDE_DIRS "include")

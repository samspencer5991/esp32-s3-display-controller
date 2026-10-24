# Portable (no ESP-IDF dependencies) - also built by sim/CMakeLists.txt.
idf_component_register(SRCS "app.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES desk pthread)

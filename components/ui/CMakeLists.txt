# Portable LVGL UI - also built by sim/CMakeLists.txt.
file(GLOB UI_SRCS "*.cpp" "fonts/*.c")
idf_component_register(SRCS ${UI_SRCS}
                       INCLUDE_DIRS "include" "."
                       REQUIRES lvgl__lvgl app_core)

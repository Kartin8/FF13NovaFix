cmake_minimum_required(VERSION 3.21)

add_library(minhook STATIC
    src/buffer.c
    src/hook.c
    src/trampoline.c
    src/hde/hde32.c)

target_include_directories(minhook PUBLIC include)
target_include_directories(minhook PRIVATE src src/hde)
set_target_properties(minhook PROPERTIES OUTPUT_NAME minhook)

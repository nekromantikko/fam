include(FetchContent)

FetchContent_Declare(
    Unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.1
)
FetchContent_MakeAvailable(Unity)

add_executable(test_apu test_apu.c)
target_link_libraries(test_apu PRIVATE fam unity)
# Unity's HEX8/HEX16 asserts cast through signed types, so expected values >= 0x80 / 0x8000 trigger C4310
target_compile_options(test_apu PRIVATE $<$<C_COMPILER_ID:MSVC>:/wd4310>)
add_test(NAME apu COMMAND test_apu)

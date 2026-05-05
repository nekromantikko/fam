include(FetchContent)

FetchContent_Declare(
    Unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.1
)
FetchContent_MakeAvailable(Unity)

add_executable(test_apu test_apu.c)
target_link_libraries(test_apu PRIVATE familib unity)
add_test(NAME apu COMMAND test_apu)

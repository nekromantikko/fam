include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.8
)
FetchContent_MakeAvailable(SDL3)

add_executable(sdl_demo sdl_demo/sdl_demo.c)
target_link_libraries(sdl_demo PRIVATE SDL3::SDL3-static)

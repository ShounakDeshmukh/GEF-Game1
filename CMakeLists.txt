cmake_minimum_required(VERSION 3.28)
project(gef_game1 VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
    engine
    GIT_REPOSITORY https://github.com/ShounakDeshmukh/GEF_engine.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(engine)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE engine::engine)
target_compile_features(game PRIVATE cxx_std_20)
set_target_properties(game PROPERTIES CXX_EXTENSIONS OFF)

target_compile_definitions(game PRIVATE
    GAME_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/"
)

target_compile_options(game PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

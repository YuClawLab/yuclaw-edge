cmake_minimum_required(VERSION 3.16)
project(yuclaw-edge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fix_gateway src/fix_gateway.cpp)
target_link_libraries(fix_gateway PRIVATE pthread)

# Install
install(TARGETS fix_gateway DESTINATION bin)

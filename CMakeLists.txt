cmake_minimum_required(VERSION 3.13.4)

project(gdswapmod VERSION 1.0.0)

find_package(Geode REQUIRED)

setup_geode_mod(${PROJECT_NAME})

if(WIN32)
    target_link_libraries(${PROJECT_NAME} wsock32 ws2_32)
endif()

cmake_minimum_required(VERSION 3.18)

project(baltamatica_mcp_bex LANGUAGES C)

find_package(Baltamatica 4.1 REQUIRED)

add_baltamatica_bex(mcp_bridge mcp_bridge.c)
set_property(TARGET mcp_bridge PROPERTY C_STANDARD 99)
set_property(TARGET mcp_bridge PROPERTY C_STANDARD_REQUIRED ON)
target_include_directories(mcp_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(mcp_bridge PRIVATE ws2_32)
endif()

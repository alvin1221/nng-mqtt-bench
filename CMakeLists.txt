cmake_minimum_required (VERSION 2.8.12)

project(nng-mqtt-bench)

find_package(nng CONFIG REQUIRED)
find_package(Threads)

add_executable(nng-mqtt-bench main.c bench.c bench.h)
target_link_libraries(nng-mqtt-bench nng)
target_link_libraries(nng-mqtt-bench ${CMAKE_THREAD_LIBS_INIT})

if(NNG_ENABLE_TLS)
    find_package(MbedTLS)
    target_link_libraries(nng-mqtt-bench mbedtls mbedx509 mbedcrypto)
    add_definitions(-DNNG_SUPP_TLS)
endif()

target_compile_definitions(nng-mqtt-bench PRIVATE NNG_ELIDE_DEPRECATED)
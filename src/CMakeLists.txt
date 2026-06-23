FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    PRIV_REQUIRES
        sony898_emulator
        driver
        freertos
        log
)

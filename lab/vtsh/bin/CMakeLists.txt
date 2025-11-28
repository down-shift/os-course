add_executable(
    vtsh
    main.c
)

target_include_directories(
    vtsh
    PUBLIC
    .
)

target_link_libraries(
    vtsh
    PRIVATE
    libvtsh
)

add_executable(
    ema-replace-int
    ema_replace_int.c
)

add_executable(
    cpu-dedup
    cpu_dedup.c
)

add_executable(
    io-load
    io_load.c
)

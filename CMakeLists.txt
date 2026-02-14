# TouchPass ZMK Module Build Configuration

if(CONFIG_ZMK_TOUCHPASS)
    zephyr_library()
    zephyr_library_sources(
        src/fingerprint_driver.c
        src/behavior_touchpass.c
    )
    zephyr_library_sources_ifdef(CONFIG_NVS src/storage.c)
    zephyr_library_sources_ifdef(CONFIG_ZMK_TOUCHPASS_SERIAL_RPC src/serial_rpc.c)

    # Include the module's headers and ZMK app headers
    zephyr_include_directories(include)
    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

    zephyr_library_link_libraries(zephyr_interface)
endif()

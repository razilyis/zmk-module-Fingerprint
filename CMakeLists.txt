# TouchPass ZMK Module Build Configuration

if(CONFIG_ZMK_TOUCHPASS)
    zephyr_library()
    zephyr_library_sources(
        src/fingerprint_driver.c
        src/behavior_touchpass.c
    )
    zephyr_library_sources_ifdef(CONFIG_FILE_SYSTEM src/storage.c)
    zephyr_library_sources_ifdef(CONFIG_ZMK_TOUCHPASS_SERIAL_RPC src/serial_rpc.c)

    # Include the module's headers and ZMK app headers
    zephyr_include_directories(include)
    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

    # Requirements: UART, File System, JSON
    zephyr_library_link_libraries(zephyr_interface)

    # LittleFS include path (lfs.h is in the external module, not in zephyr_interface)
    if(CONFIG_FILE_SYSTEM_LITTLEFS AND DEFINED ZEPHYR_LITTLEFS_MODULE_DIR)
        zephyr_library_include_directories(${ZEPHYR_LITTLEFS_MODULE_DIR})
    endif()
endif()

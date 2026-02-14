# TouchPass ZMK Module Build Configuration

if(CONFIG_ZMK_TOUCHPASS)
    zephyr_library()
    zephyr_library_sources(
        src/fingerprint_driver.c
        src/storage.c
        src/serial_rpc.c
        src/behavior_touchpass.c
    )
    
    # Include the module's headers and ZMK app headers
    zephyr_include_directories(include)
    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
    
    # Requirements: UART, File System, JSON
    zephyr_library_link_libraries(zephyr_interface)
endif()

# TouchPass ZMK Module Build Configuration

if(CONFIG_ZMK_TOUCHPASS)
    zephyr_library()
    zephyr_library_sources(
        src/fingerprint_driver.c
        src/storage.c
        src/serial_rpc.c
        src/behavior_touchpass.c
    )
    
    # Include the module's headers
    zephyr_include_directories(include)
    
    # Requirements: UART, File System, JSON
    zephyr_library_link_libraries(zephyr_interface)
endif()

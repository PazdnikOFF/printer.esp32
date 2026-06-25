# esp_tinyusb is a managed component (espressif/esp_tinyusb via idf_component.yml).
# Its CMake name is "espressif__esp_tinyusb" when downloaded by IDF Component Manager,
# or "esp_tinyusb" if a local copy is present.
if(esp_tinyusb IN_LIST build_components)
    set(tinyusb_dep esp_tinyusb)
else()
    set(tinyusb_dep espressif__esp_tinyusb)
endif()

idf_component_register(
    SRCS
        "Sony898Printer.cpp"
        "sony898_emulator.c"
        "sony898_usb.c"
        "sony898_parser.c"
        "sony898_status.c"
        "sony898_image.c"
        "sony898_sensors.c"
        "sony898_printer_iface.c"
        "sony898_counter.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        ${tinyusb_dep}
        log
        freertos
        heap
        nvs_flash
)

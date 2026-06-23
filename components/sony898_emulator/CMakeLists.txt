idf_component_register(
    SRCS
        "sony898_usb.c"
        "sony898_parser.c"
        "sony898_status.c"
        "sony898_image.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        tinyusb
        log
        esp_common
        freertos
        heap
)

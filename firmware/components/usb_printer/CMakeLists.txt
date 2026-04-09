idf_component_register(
    SRCS "usb_printer.c"
    INCLUDE_DIRS "include"
    REQUIRES usb log freertos
    PRIV_REQUIRES esp_timer
)

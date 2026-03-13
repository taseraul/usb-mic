idf_component_register(
    SRCS "main.c" "usb_descriptors.c"
    INCLUDE_DIRS "../include"
    REQUIRES driver esp_ringbuf esp_tinyusb
)

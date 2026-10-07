idf_component_register(
    SRCS
        "app_main.c"
        "oled.c"
        "audio.c"
        "usb.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        fatfs
        minimp3
        esp_timer
        # In IDF 6.x the I2C and I2S drivers were split out of `driver` into
        # their own components; their headers (driver/i2c_master.h,
        # driver/i2s_std.h) need these listed explicitly.
        esp_driver_i2c
        esp_driver_i2s
        # usb_host_msc is a managed dependency (see idf_component.yml). It
        # provides usb/usb_host.h + usb/msc_host_vfs.h and pulls in `usb_host`.
        usb_host_msc
)


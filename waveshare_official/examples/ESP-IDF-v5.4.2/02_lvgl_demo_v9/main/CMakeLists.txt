idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES lvgl__lvgl waveshare__esp32_s3_touch_amoled_2_06 nvs_flash)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

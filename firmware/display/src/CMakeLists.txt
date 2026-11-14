FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)

idf_component_register(SRCS ${app_sources}
                       REQUIRES driver esp_lcd esp_wifi esp_netif esp_event nvs_flash
                                esp_http_server app_update esp_timer esp_psram esp_hw_support
                                freertos log)

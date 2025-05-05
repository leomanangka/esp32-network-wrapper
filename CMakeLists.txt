idf_component_register(
  SRCS "network_wrapper.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi nvs_flash
)

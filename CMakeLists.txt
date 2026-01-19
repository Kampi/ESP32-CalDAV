idf_component_register(
    SRCS "src/caldav_client.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp-tls
)

# Add version definitions to the component
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ESP32_CALDAV_LIB_MAJOR=${ESP32_CALDAV_LIB_MAJOR}
    ESP32_CALDAV_LIB_MINOR=${ESP32_WEBDAV_LIB_MINOR}
    ESP32_CALDAV_LIB_BUILD=${ESP32_WEBDAV_LIB_BUILD}
)
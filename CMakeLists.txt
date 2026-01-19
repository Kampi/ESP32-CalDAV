idf_component_register(
    SRCS "src/caldav_client.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp-tls
)

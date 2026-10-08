idf_component_register(
    SRCS "src/sml.cpp" "src/sml_adapter.cpp"
    INCLUDE_DIRS "include" "src"
    REQUIRES sml_transport
)

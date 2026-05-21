idf_component_register(
SRCS
     "./main.c"
     "./ble/ble_client.c"
     "./control/control_callback.c"

INCLUDE_DIRS
     "."

REQUIRES bt driver nvs_flash
)


set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

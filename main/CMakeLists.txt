idf_component_register(SRCS "main.c" "WiFi/wifi.c" "Sensor/sensor.c" "MQTT/mqtt.c"
                    INCLUDE_DIRS "." "Sensor"
                    EMBED_TXTFILES "Certs/AmazonRootCA1.pem"
                                   "Certs/device.pem.crt"
                                   "Certs/private.pem.key")



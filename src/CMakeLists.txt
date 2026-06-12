idf_component_register(SRCS "main.cpp"
                            "app_camera.cpp"
                            "app_capture_uploader.cpp"
                            "app_http_server.cpp"
                            "app_person_detect.cpp"
                            "app_wifi_ap.cpp"
                            "app_wifi_sta.cpp"
                       INCLUDE_DIRS "../include"
                       REQUIRES esp-dl pedestrian_detect esp_driver_rmt esp_http_client)

# DEKB shared modules (buzzer + indicator)
zephyr_library()
zephyr_library_sources(src/de_indicator.c src/de_buzzer.c)
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

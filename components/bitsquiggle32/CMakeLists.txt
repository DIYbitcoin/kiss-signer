# BitSquiggles (maggo83) as an ESP-IDF component. See README.md for the pin and
# for why the two source files are byte-identical copies of upstream.
idf_component_register(SRCS "bitsquiggle32.c"
                       INCLUDE_DIRS ".")

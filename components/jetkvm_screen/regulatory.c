#include "regulatory.h"

#include "jetkvm_board_config.h"

const char *const jetkvm_regulatory_lines[] = {
#if JETKVM_BOARD_WIFI
    /* The Wi-Fi board's radio is the certified ESP32-C5-MINI-1 module. */
    "Contains FCC ID: 2AC7Z-ESPC5MINI1",
#endif
    "This device complies with part 15 of the FCC Rules. Operation is subject to the "
    "following two conditions: (1) This device may not cause harmful interference, and "
    "(2) this device must accept any interference received, including interference that "
    "may cause undesired operation.",
};

const size_t jetkvm_regulatory_line_count =
    sizeof(jetkvm_regulatory_lines) / sizeof(jetkvm_regulatory_lines[0]);

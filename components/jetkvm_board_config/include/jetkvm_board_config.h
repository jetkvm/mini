#pragma once

/*
 * Board signals in this file are copied from the connector nets. Keep the two
 * variants explicit: do not infer unlisted connector functions from a signal
 * name or from the peripheral's usual pinout.
 */

#if !defined(JETKVM_BOARD_WIFI) || !defined(JETKVM_BOARD_ETHERNET)
#error "JetKVM board variant compile definitions are missing"
#endif

#if JETKVM_BOARD_WIFI == JETKVM_BOARD_ETHERNET
#error "Exactly one JetKVM board variant must be selected"
#endif

#define JETKVM_SCREEN_PIN_MOSI 3
#define JETKVM_SCREEN_PIN_BACKLIGHT 4
#define JETKVM_SCREEN_PIN_SCLK 13

/* Front push buttons, active low behind external 10 kOhm pull-ups. SW3 is
 * on GPIO23 on the Ethernet board and GPIO17 on the Wi-Fi board (see
 * docs/hardware-variants.md). */
#define JETKVM_BUTTON_SW2_PIN 47
#if JETKVM_BOARD_WIFI
#define JETKVM_BUTTON_SW3_PIN 17
#else
#define JETKVM_BUTTON_SW3_PIN 23
#endif

/* With CONFIG_JETKVM_SCREEN_BUTTONS off, SW3 toggles the HDMI preview mode. */
#define JETKVM_VIDEO_MODE_BUTTON_PIN JETKVM_BUTTON_SW3_PIN

/* Hardware SKU sent to the release API (?sku=) and the cloud (X-Device-SKU).
 * One per board variant because the variants are separate images; the cloud
 * only serves an image built for the SKU that asks. */
#if JETKVM_BOARD_WIFI
#define JETKVM_DEVICE_SKU "jetkvm-mini-wireless"
#else
#define JETKVM_DEVICE_SKU "jetkvm-mini-ethernet"
#endif

#if JETKVM_BOARD_WIFI
/* Netlist FPC2: LCD_DC, LCD_RST and LCD_CS. */
#define JETKVM_SCREEN_PIN_DC 14
#define JETKVM_SCREEN_PIN_RST 15
#define JETKVM_SCREEN_PIN_CS 16

/* Netlist FPC1: C5 control and four-bit SDIO host connection. */
#define JETKVM_C5_PIN_BOOT 7
#define JETKVM_C5_PIN_EN 9
#define JETKVM_C5_PIN_SDIO_CMD 18
#define JETKVM_C5_PIN_SDIO_CLK 19
#define JETKVM_C5_PIN_SDIO_D0 20
#define JETKVM_C5_PIN_SDIO_D1 21
#define JETKVM_C5_PIN_SDIO_D2 22
#define JETKVM_C5_PIN_SDIO_D3 23
#else
/* Ethernet-board screen connector nets. */
#define JETKVM_SCREEN_PIN_DC 20
#define JETKVM_SCREEN_PIN_RST 21
#define JETKVM_SCREEN_PIN_CS 22
#endif

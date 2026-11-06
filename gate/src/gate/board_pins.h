/* XIAO ESP32-C5 + Seeed Wio-SX1262 for XIAO, soldered to the C5's left header
 * row. Silkscreen label -> GPIO from the Arduino core's XIAO_ESP32C5 variant.
 * The module's pin labels, read with the C5's USB port at the bottom, give
 * RF-SW=D4, NSS=D3, RST=D2, BUSY=D1, DIO1=D0 (Meshtastic's SEEED_XIAO_NRF_WIO_BTB
 * pin set). Verified on hardware: CS, BUSY and DIO1 (the chip answers "SX126x"
 * and raises DIO1 on TX done). RST and RXEN are as labelled, not separately
 * verified. SPI is on D8/D9/D10. Override with -DLORA_xxx=<gpio>. */
#pragma once

#define XIAO_C5_D0   1
#define XIAO_C5_D1   0
#define XIAO_C5_D2   25
#define XIAO_C5_D3   7
#define XIAO_C5_D4   23
#define XIAO_C5_D5   24
#define XIAO_C5_D8   8   /* SCK  */
#define XIAO_C5_D9   9   /* MISO */
#define XIAO_C5_D10  10  /* MOSI */

#ifndef LORA_SCK
#define LORA_SCK   XIAO_C5_D8
#endif
#ifndef LORA_MISO
#define LORA_MISO  XIAO_C5_D9
#endif
#ifndef LORA_MOSI
#define LORA_MOSI  XIAO_C5_D10
#endif
#ifndef LORA_CS
#define LORA_CS    XIAO_C5_D3
#endif
#ifndef LORA_DIO1
#define LORA_DIO1  XIAO_C5_D0
#endif
#ifndef LORA_RST
#define LORA_RST   XIAO_C5_D2
#endif
#ifndef LORA_BUSY
#define LORA_BUSY  XIAO_C5_D1
#endif
#ifndef LORA_RXEN
#define LORA_RXEN  XIAO_C5_D4
#endif

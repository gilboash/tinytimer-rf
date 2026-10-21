/* XIAO ESP32-C5 + Wio-SX1262 pin-header module (the "for XIAO" one, SKU
 * 113010003). Silkscreen label -> GPIO from Seeed's XIAO ESP32-C5 pin table;
 * the module's control pins are the ones the power-meter project verified on
 * the S3 (CS=D4, DIO1=D1, RST=D2, BUSY=D3, RXEN=D5, SPI on D8/D9/D10).
 * Override any of them with -DLORA_xxx=<gpio> in platformio.ini. */
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
#define LORA_CS    XIAO_C5_D4
#endif
#ifndef LORA_DIO1
#define LORA_DIO1  XIAO_C5_D1
#endif
#ifndef LORA_RST
#define LORA_RST   XIAO_C5_D2
#endif
#ifndef LORA_BUSY
#define LORA_BUSY  XIAO_C5_D3
#endif
#ifndef LORA_RXEN
#define LORA_RXEN  XIAO_C5_D5
#endif

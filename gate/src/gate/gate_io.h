/* USB output of passes and the live level stream, and the hand-off of passes
 * to the LoRa task. GPL-3.0-or-later. */
#pragma once
void gate_io_start(void);
void gate_io_set_stream_hz(unsigned hz);
unsigned gate_io_stream_hz(void);

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the shared full-duplex I2S1 bus.
 *
 * TX is configured for MAX98357A (DOUT), RX is configured for INMP441 (DIN),
 * and both channels share the same BCLK + WS signals.
 */
esp_err_t i2s_shared_bus_init(void);

/**
 * Deinitialize shared I2S channels.
 *
 * Use only when no component is using either TX or RX channel anymore.
 */
void i2s_shared_bus_deinit(void);

/**
 * Query shared bus initialization state.
 */
bool i2s_shared_bus_is_initialized(void);

/**
 * Get TX channel handle (MAX98357A path).
 * Returns NULL until i2s_shared_bus_init() succeeds.
 */
i2s_chan_handle_t i2s_shared_bus_get_tx_channel(void);

/**
 * Get RX channel handle (INMP441 path).
 * Returns NULL until i2s_shared_bus_init() succeeds.
 */
i2s_chan_handle_t i2s_shared_bus_get_rx_channel(void);

#ifdef __cplusplus
}
#endif

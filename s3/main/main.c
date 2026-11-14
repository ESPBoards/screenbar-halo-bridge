/* SPDX-License-Identifier: GPL-3.0-only */
/* The radio bridge: the C3 says how the lamp should look, the S3 makes it so
 * and reports what the remote does.
 */
#include "bridge.h"
#include "radio.h"

void app_main(void)
{
    bridge_init();
    radio_start(bridge_state, bridge_heard);
    bridge_start();
}

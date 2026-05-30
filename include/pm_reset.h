// include/pm_reset.h — BCM2711 (Pi 4) and BCM2712 (Pi 5) power-management
// watchdog reset.  Wires `cmd_reboot` and HTTP `POST /reboot` to an
// actual SoC reset — no more "power-cycle the board" stub.
//
// Tested against the Pi 4 BCM2711 PM block at PM_BASE = 0xFE100000.
// Pi 5 (BCM2712) uses a different SoC reset path (RP1 GPIO) and is
// stubbed out here; until that's filled in, `reboot` on Pi 5 still
// requires a power cycle.

#ifndef XINU_RPI4_PM_RESET_H
#define XINU_RPI4_PM_RESET_H

/* Trigger an SoC reset via the BCM2711 watchdog.  Never returns. */
void pm_reset(void);

#endif

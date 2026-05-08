// ethernetif.h — lwIP netif init + polling entry points.
#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/netif.h"

// Called by netif_add() to initialise the LiteEth netif.
err_t ethernetif_init(struct netif *netif);

// Call this from the main loop (or eth_task) to drain pending RX frames.
void ethernetif_input(struct netif *netif);

#endif // ETHERNETIF_H

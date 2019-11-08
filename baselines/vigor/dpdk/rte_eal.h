#pragma once

#include <stdbool.h>

#include <tn_dpdk.h>


// We do not accept real DPDK arguments, but instead a list of PCI addresses for devices
int rte_eal_init(int argc, char **argv)
{
	bool foundEnd = false;
	int count = 0;
	// note that argv[0] is the program name
	for (int n = 1; n < argc; n++) {
		if (argv[n][0] == '-' && argv[n][1] == '-') {
			foundEnd = true;
			break;
		}
		count++;
	}

	if (!tn_util_parse_pci(count, argv + 1, tn_dpdk_pci_devices)) {
		return -1;
	}

	for (int n = 0; n < count; n++) {
		if (!tn_net_device_init(tn_dpdk_pci_devices[n], &(tn_dpdk_devices[n].device))) {
			return -1;
		}

		if (!tn_net_pipe_init(&(tn_dpdk_devices[n].pipe))) {
			return -1;
		}

		if (!tn_net_pipe_set_receive(tn_dpdk_devices[n].pipe, tn_dpdk_devices[n].device, 0)) {
			return -1;
		}
	}

#ifdef ASSUME_ONE_WAY
	if (count != 2) {
		return -1;
	}

	for (int n = 0; n < count; n++) {
		if (!tn_net_pipe_add_send(tn_dpdk_devices[n].pipe, tn_dpdk_devices[1 - n].device, n)) {
			return -1;
		}
	}
#else
	for (int n = 0; n < count; n++) {
		for (int d = 0; d < count; d++) {
			if (!tn_net_pipe_add_send(tn_dpdk_devices[n].pipe, tn_dpdk_devices[d].device, n)) {
				return -1;
			}
		}
	}
#endif


	tn_dpdk_devices_count = count;

	return count + 1 + foundEnd; // consumed count, plus argv[0], plus potentially the -- marker
}

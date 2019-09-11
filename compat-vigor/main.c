// TinyNF
#include "net/network.h"
#include "util/parse.h"

// Vigor
#include "nf.h"

#include <stdbool.h>


#define DEVICES_MAX_COUNT 128u

static uint16_t devices_count;

static uint16_t current_device;
static uint16_t compat_packet_handler(uint8_t* packet, uint16_t packet_length, bool* send_list)
{
	vigor_time_t vigor_now = current_time();
	int vigor_output = nf_process(current_device, packet, packet_length, vigor_now);
	if (vigor_output == FLOOD_FRAME) {
		for (uint16_t n = 0; n < devices_count; n++) {
			send_list[n] = true;
		}
	} else if (vigor_output == current_device) {
		// Nothing; this means "drop", Vigor has no notion of sending back to the same device
	} else {
		send_list[vigor_output] = true;
	}
}

int main(int argc, char** argv)
{
	devices_count = (uint16_t) (argc - 1);
	if (devices_count == 0 || devices_count > DEVICES_MAX_COUNT) {
		return 1;
	}

	// TinyNF init
	struct tn_pci_device* pci_devices[DEVICES_MAX_COUNT];
	if (!tn_util_parse_pci(devices_count, argv + 1, pci_devices)) {
		return 2;
	}
	struct tn_net_device* devices[DEVICES_MAX_COUNT];
	struct tn_net_pipe* pipes[DEVICES_MAX_COUNT];
	for (uint16_t n = 0; n < devices_count; n++) {
		if (!tn_net_device_init(pci_devices[n], &(devices[n])) {
			return 1000 + n;
		}
		if (!tn_net_pipe_init(&(pipes[n])) {
			return 2000 + n;
		}
		if (!tn_net_pipe_set_receive(pipes[n], devices[n], 0)) {
			return 3000 + n;
		}
	}
	for (uint16_t p = 0; p < devices_count; p++) {
		for (uint16_t q = 0; q < devices_count; q++) {
			if (!tn_net_pipe_add_send(pipes[p], devices[q], p)) {
				return 4000 + p * q;
			}
		}
	}

	// Vigor init
	nf_config_init();
	nf_init();

	// Compat layer
	while(true) {
		for (current_device = 0; current_device < devices_count; current_device++) {
			tn_net_pipe_run_step(pipes[current_device], compat_packet_handler);
		}
	}
}

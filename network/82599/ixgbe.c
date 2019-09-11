#include "network/network.h"

#include "arch/dca.h"
#include "arch/endian.h"
#include "os/memory.h"
#include "os/time.h"
#include "util/log.h"

#include <assert.h>


// ASSUMPTIONS
// ===========
// We make the following assumptions, which we later refer to by name:
// CACHE: The cache line size is 64 bytes
// CRC: We want CRC stripped when receiving and generated on the entire packet when sending
// DCA: We want Direct Cache Access (DCA), for performance
// DROP: We want to drop packets if we can't process them fast enough, for predictable behavior
// NOCARE: We do not care about the following:
//         - Statistics
//         - Receive Side Coalescing (RSC)
//         - NFS
// NOWANT: We do not want the following:
//         - Flexible filters
//         - VLAN handling
//         - Multicast filtering
//         - Receive checksum offloading
//         - Receive side scaling (RSS)
//         - 5-tuple filtering
//         - L2 ethertype filtering
//         - SR-IO
//         - VMDq
//         - Jumbo packet handling
//         - LinkSec packet handling
//         - Loopback
//         - Global double VLAN mode
//         - Interrupts
//         - Debugging features
// PCIBARS: The PCI BARs have been pre-configured to valid values, and will not collide with any other memory we may handle
// PCIBRIDGES: All PCI bridges can be ignored, i.e. they do not enforce power savings modes or any other limitations
// REPORT: We prefer more error reporting when faced with an explicit choice, but do not attempt to do extra configuration for this
// TRUST: We trust the defaults for low-level hardware details
// TXPAD: We want all sent frames to be at least 64 bytes

// Maximum number of send queues assigned to a pipe. Not based on any spec, can be redefined at will
#define IXGBE_PIPE_MAX_SENDS 4u
// Scheduling period for bookkeeping and sending. Not based on any spec, can be redefined at will as long as it's a power of 2 for fast modulo
#define IXGBE_PIPE_SCHEDULING_PERIOD 512
static_assert((IXGBE_PIPE_SCHEDULING_PERIOD & (IXGBE_PIPE_SCHEDULING_PERIOD - 1)) == 0, "Scheduling period must be a power of 2");

// Section 7.1.2.5 L3/L4 5-tuple Filters:
// 	"There are 128 different 5-tuple filter configuration registers sets"
#define IXGBE_5TUPLE_FILTERS_COUNT 128u
// Section 7.3.1 Interrupts Registers:
//	"These registers are extended to 64 bits by an additional set of two registers.
//	 EICR has an additional two registers EICR(1)... EICR(2) and so on for the EICS, EIMS, EIMC, EIAM and EITR registers."
#define IXGBE_INTERRUPT_REGISTERS_COUNT 2u
// Section 7.10.3.10 Switch Control:
//	"Multicast Table Array (MTA) - a 4 Kb array that covers all combinations of 12 bits from the MAC destination address."
#define IXGBE_MULTICAST_TABLE_ARRAY_SIZE (4u * 1024u)
// Section 8.2.3.8.7 Split Receive Control Registers: "Receive Buffer Size for Packet Buffer. Value can be from 1 KB to 16 KB"
// Section 7.2.3.2.2 Legacy Transmit Descriptor Format: "The maximum length associated with a single descriptor is 15.5 KB"
#define IXGBE_PACKET_SIZE_MAX (2u * 1024u)
// Section 7.1.1.1.1 Unicast Filter:
// 	"The Ethernet MAC address is checked against the 128 host unicast addresses"
#define IXGBE_RECEIVE_ADDRS_COUNT 128u
// Section 1.3 Features Summary:
// 	"Number of Rx Queues (per port): 128"
#define IXGBE_RECEIVE_QUEUES_COUNT 128u
// Section 7.2.3.3 Transmit Descriptor Ring:
// "Transmit Descriptor Length register (TDLEN 0-127) - This register determines the number of bytes allocated to the circular buffer. This value must be 0 modulo 128."
// This must be a power of 2 to enable fast modulo
#define IXGBE_RING_SIZE 1024u
static_assert((IXGBE_RING_SIZE & (IXGBE_RING_SIZE - 1)) == 0, "Ring size must be a power of 2");
// 	"Number of Tx Queues (per port): 128"
#define IXGBE_SEND_QUEUES_COUNT 128u
// Section 7.1.2 Rx Queues Assignment:
// 	"Packets are classified into one of several (up to eight) Traffic Classes (TCs)."
#define IXGBE_TRAFFIC_CLASSES_COUNT 8u
// Section 7.10.3.10 Switch Control:
// 	"Unicast Table Array (PFUTA) - a 4 Kb array that covers all combinations of 12 bits from the MAC destination address"
#define IXGBE_UNICAST_TABLE_ARRAY_SIZE (4u * 1024u)
// Section 7.10.3.2 Pool Selection:
// 	"64 shared VLAN filters"
#define IXGBE_VLAN_FILTER_COUNT 64u


// ---------
// Utilities
// ---------

// Overload macros for up to 5 args, see https://stackoverflow.com/a/11763277/3311770
#define GET_MACRO(_1, _2, _3, _4, _5, NAME, ...) NAME

// Bit tricks; note that bit indices start at 0!
#define BIT(n) (1u << (n))
#define BITL(n) (1ull << (n))
#define BITS(start, end) (((end) == 31 ? 0u : (0xFFFFFFFFu << ((end) + 1))) ^ (0xFFFFFFFFu << (start)))
#define TRAILING_ZEROES(n) __builtin_ctzll(n)

// Like if(...) but polls with a timeout, and executes the body only if the condition is still true after the timeout
static bool timed_out;
#define IF_AFTER_TIMEOUT(timeout_in_us, condition) \
		timed_out = true; \
		tn_sleep_us((timeout_in_us) % 10); \
		for (uint8_t i = 0; i < 10; i++) { \
			if (!condition) { \
				timed_out = false; \
				break; \
			} \
			tn_sleep_us((timeout_in_us) / 10); \
		} \
		if (timed_out)


// ---------------------
// Operations on the NIC
// ---------------------

// Register primitives
static uint32_t ixgbe_reg_read(const uintptr_t addr, const uint32_t reg)
{
	uint32_t val_le = *((volatile uint32_t*)(addr + reg));
	uint32_t result = tn_le_to_cpu(val_le);
	TN_DEBUG("IXGBE read (addr 0x%016" PRIxPTR "): 0x%08" PRIx32 " -> 0x%08" PRIx32, addr, reg, result);
	return result;
}
static void ixgbe_reg_write_raw(const uintptr_t reg_addr, const uint32_t value)
{
	*((volatile uint32_t*)reg_addr) = tn_cpu_to_le(value);
}
static void ixgbe_reg_write(const uintptr_t addr, const uint32_t reg, const uint32_t value)
{
	ixgbe_reg_write_raw(addr + reg, value);
	TN_DEBUG("IXGBE write (addr 0x%016" PRIxPTR "): 0x%08" PRIx32 " := 0x%08" PRIx32, addr, reg, value);
}

#define IXGBE_REG_READ3(addr, reg, idx) ixgbe_reg_read(addr, IXGBE_REG_##reg(idx))
#define IXGBE_REG_READ4(addr, reg, idx, field) ((IXGBE_REG_READ3(addr, reg, idx) & IXGBE_REG_##reg##_##field) >> TRAILING_ZEROES(IXGBE_REG_##reg##_##field))
#define IXGBE_REG_READ(...) GET_MACRO(__VA_ARGS__, _UNUSED, IXGBE_REG_READ4, IXGBE_REG_READ3, _UNUSED)(__VA_ARGS__)
#define IXGBE_REG_WRITE4(addr, reg, idx, value) ixgbe_reg_write(addr, IXGBE_REG_##reg(idx), value)
#define IXGBE_REG_WRITE5(addr, reg, idx, field, value) ixgbe_reg_write(addr, IXGBE_REG_##reg(idx), ((IXGBE_REG_READ(addr, reg, idx) & ~IXGBE_REG_##reg##_##field) | (((value) << TRAILING_ZEROES(IXGBE_REG_##reg##_##field)) & IXGBE_REG_##reg##_##field)))
#define IXGBE_REG_WRITE(...) GET_MACRO(__VA_ARGS__, IXGBE_REG_WRITE5, IXGBE_REG_WRITE4, _UNUSED)(__VA_ARGS__)
#define IXGBE_REG_CLEARED(addr, reg, idx, field) (IXGBE_REG_READ(addr, reg, idx, field) == 0u)
#define IXGBE_REG_CLEAR3(addr, reg, idx) IXGBE_REG_WRITE(addr, reg, idx, 0U)
#define IXGBE_REG_CLEAR4(addr, reg, idx, field) IXGBE_REG_WRITE(addr, reg, idx, (IXGBE_REG_READ(addr, reg, idx) & ~IXGBE_REG_##reg##_##field))
#define IXGBE_REG_CLEAR(...) GET_MACRO(__VA_ARGS__, _UNUSED, IXGBE_REG_CLEAR4, IXGBE_REG_CLEAR3, _UNUSED)(__VA_ARGS__)
#define IXGBE_REG_SET(addr, reg, idx, field) IXGBE_REG_WRITE(addr, reg, idx, (IXGBE_REG_READ(addr, reg, idx) | IXGBE_REG_##reg##_##field))

#define IXGBE_PCIREG_READ(pci_dev, reg) tn_pci_read(pci_dev, IXGBE_PCIREG_##reg)
#define IXGBE_PCIREG_CLEARED(pci_dev, reg, field) ((IXGBE_PCIREG_READ(pci_dev, reg) & IXGBE_PCIREG_##reg##_##field) == 0u)
#define IXGBE_PCIREG_SET(pci_dev, reg, field) tn_pci_write(pci_dev, IXGBE_PCIREG_##reg, (IXGBE_PCIREG_READ(pci_dev, reg) | IXGBE_PCIREG_##reg##_##field))


// -------------------------------------------------------------
// PCI registers, in alphabetical order, along with their fields
// -------------------------------------------------------------

// Section 9.3.2 PCIe Configuration Space Summary: "0x10 Base Address Register 0" (32 bit), "0x14 Base Address Register 1" (32 bit)
#define IXGBE_PCIREG_BAR0_LOW 0x10u
#define IXGBE_PCIREG_BAR0_HIGH 0x14u

// Section 9.3.3.3 Command Register (16 bit)
// Section 9.3.3.4 Status Register (16 bit, unused)
#define IXGBE_PCIREG_COMMAND 0x04u
#define IXGBE_PCIREG_COMMAND_MEMORY_ACCESS_ENABLE BIT(1)
#define IXGBE_PCIREG_COMMAND_BUS_MASTER_ENABLE BIT(2)
#define IXGBE_PCIREG_COMMAND_INTERRUPT_DISABLE BIT(10)

// Section 9.3.10.6 Device Status Register (16 bit)
// Section 9.3.10.7 Link Capabilities Register (16 bit, unused)
#define IXGBE_PCIREG_DEVICESTATUS 0xAAu
#define IXGBE_PCIREG_DEVICESTATUS_TRANSACTIONPENDING BIT(5)

// Section 9.3.3.1 Vendor ID Register (16 bit)
// Section 9.3.3.2 Device ID Register (16 bit)
#define IXGBE_PCIREG_ID 0x00u

// Section 9.3.7.1.4 Power Management Control / Status Register (16 bit)
// Section 9.3.7.1.5 PMCSR_BSE Bridge Support Extensions Register (8 bit, hardwired to 0)
// Section 9.3.7.1.6 Data Register (8 bit, unused)
#define IXGBE_PCIREG_PMCSR 0x44u
#define IXGBE_PCIREG_PMCSR_POWER_STATE BITS(0,1)

// ---------------------------------------------------------
// Registers, in alphabetical order, along with their fields
// ---------------------------------------------------------

// Section 8.2.3.1.1 Device Control Register
#define IXGBE_REG_CTRL(_) 0x00000u
#define IXGBE_REG_CTRL_MASTER_DISABLE BIT(2)
#define IXGBE_REG_CTRL_LRST BIT(3)

// Section 8.2.3.1.3 Extended Device Control Register
#define IXGBE_REG_CTRLEXT(_) 0x00018u
#define IXGBE_REG_CTRLEXT_NSDIS BIT(16)

// Section 8.2.3.11.3 DCA Control Register
#define IXGBE_REG_DCACTRL(_) 0x11074u
#define IXGBE_REG_DCACTRL_DCA_DIS BIT(0)
#define IXGBE_REG_DCACTRL_DCA_MODE BITS(1,4)

// Section 8.2.3.11.1 Rx DCA Control Register
#define IXGBE_REG_DCARXCTRL(n) ((n) <= 63u ? (0x0100Cu + 0x40u*(n)) : (0x0D00Cu + 0x40u*((n)-64u)))
#define IXGBE_REG_DCARXCTRL_RX_DESCRIPTOR_DCA_EN BIT(5)
#define IXGBE_REG_DCARXCTRL_RX_PAYLOAD_DCA_EN BIT(7)
// This bit is reserved, has no name, but must be used anyway
#define IXGBE_REG_DCARXCTRL_UNKNOWN BIT(12)
#define IXGBE_REG_DCARXCTRL_CPUID BITS(24,31)

// Section 8.2.3.11.2 Tx DCA Control Registers
#define IXGBE_REG_DCATXCTRL(n) (0x0600Cu + 0x40u*(n))
#define IXGBE_REG_DCATXCTRL_TX_DESCRIPTOR_DCA_EN BIT(5)
#define IXGBE_REG_DCATXCTRL_TX_DESC_WB_RO_EN BIT(11)
#define IXGBE_REG_DCATXCTRL_CPUID BITS(24,31)

// Section 8.2.3.9.2 DMA Tx Control
#define IXGBE_REG_DMATXCTL(_) 0x04A80u
#define IXGBE_REG_DMATXCTL_TE BIT(0)

// Section 8.2.3.9.1 DMA Tx TCP Max Allow Size Requests
#define IXGBE_REG_DTXMXSZRQ(_) 0x08100u
#define IXGBE_REG_DTXMXSZRQ_MAX_BYTES_NUM_REQ BITS(0,11)

// Section 8.2.3.2.1 EEPROM/Flash Control Register
#define IXGBE_REG_EEC(_) 0x10010u
#define IXGBE_REG_EEC_EE_PRES BIT(8)
#define IXGBE_REG_EEC_AUTO_RD BIT(9)

// Section 8.2.3.5.9 Extended Interrupt Mask Clear Registers
#define IXGBE_REG_EIMC(n) (0x00AB0u + 4u*(n))
#define IXGBE_REG_EIMC_MASK BITS(0,31)

// Section 8.2.3.3.4 Flow Control Receive Threshold High
#define IXGBE_REG_FCRTH(n) (0x03260u + 4u*(n))
#define IXGBE_REG_FCRTH_RTH BITS(5,18)

// Section 8.2.3.7.1 Filter Control Register (FCTRL)
#define IXGBE_REG_FCTRL(_) 0x05080u
#define IXGBE_REG_FCTRL_MPE BIT(8)
#define IXGBE_REG_FCTRL_UPE BIT(9)

// Section 8.2.3.7.19 Five tuple Queue Filter
#define IXGBE_REG_FTQF(n) (0x0E600u + 4u*(n))
#define IXGBE_REG_FTQF_QUEUE_ENABLE BIT(31)

// Section 8.2.3.4.10 Firmware Semaphore Register
#define IXGBE_REG_FWSM(_) 0x10148u
#define IXGBE_REG_FWSM_EXT_ERR_IND BITS(19,24)

// Section 8.2.3.4.12 PCIe Control Extended Register
#define IXGBE_REG_GCREXT(_) 0x11050u
#define IXGBE_REG_GCREXT_BUFFERS_CLEAR_FUNC BIT(30)

// Section 8.2.3.22.8 MAC Core Control 0 Register
#define IXGBE_REG_HLREG0(_) 0x04240u
#define IXGBE_REG_HLREG0_LPBK BIT(15)

// Section 8.2.3.22.34 MAC Flow Control Register
#define IXGBE_REG_MFLCN(_) 0x04294u
#define IXGBE_REG_MFLCN_RFCE BIT(3)

// Section 8.2.3.7.10 MAC Pool Select Array
#define IXGBE_REG_MPSAR(n) (0x0A600u + 4u*(n))

// Section 8.2.3.7.7 Multicast Table Array
#define IXGBE_REG_MTA(n) (0x05200u + 4u*(n))

// Section 8.2.3.27.17 PF Unicast Table Array
#define IXGBE_REG_PFUTA(n) (0x0F400u + 4u*(n))

// Section 8.2.3.27.15 PF VM VLAN Pool Filter
#define IXGBE_REG_PFVLVF(n) (0x0F100u + 4u*(n))

// Section 8.2.3.27.16 PF VM VLAN Pool Filter Bitmap
#define IXGBE_REG_PFVLVFB(n) (0x0F200u + 4u*(n))

// Section 8.2.3.8.2 Receive Descriptor Base Address High
#define IXGBE_REG_RDBAH(n) ((n) <= 63u ? (0x01004u + 0x40u*(n)) : (0x0D004u + 0x40u*((n)-64u)))

// Section 8.2.3.8.1 Receive Descriptor Base Address Low
#define IXGBE_REG_RDBAL(n) ((n) <= 63u ? (0x01000u + 0x40u*(n)) : (0x0D000u + 0x40u*((n)-64u)))

// Section 8.2.3.8.3 Receive Descriptor Length
#define IXGBE_REG_RDLEN(n) ((n) <= 63u ? (0x01008u + 0x40u*(n)) : (0x0D008 + 0x40u*((n)-64u)))

// Section 8.2.3.8.8 Receive DMA Control Register
// INTERPRETATION: Bit 0, which is not mentioned in the table, is reserved
#define IXGBE_REG_RDRXCTL(_) 0x02F00u
#define IXGBE_REG_RDRXCTL_CRC_STRIP BIT(1)
#define IXGBE_REG_RDRXCTL_DMAIDONE BIT(3)
#define IXGBE_REG_RDRXCTL_RSCFRSTSIZE BITS(17,24)
#define IXGBE_REG_RDRXCTL_RSCACKC BIT(25)
#define IXGBE_REG_RDRXCTL_FCOE_WRFIX BIT(26)

// Section 8.2.3.8.5 Receive Descriptor Tail
#define IXGBE_REG_RDT(n) ((n) <= 63u ? (0x01018u + 0x40u*(n)) : (0x0D018u + 0x40u*((n)-64u)))

// Section 8.2.3.10.2 DCB Transmit Descriptor Plane Control and Status
#define IXGBE_REG_RTTDCS(_) 0x04900u
#define IXGBE_REG_RTTDCS_ARBDIS BIT(6)

// Section 8.2.3.8.10 Receive Control Register
#define IXGBE_REG_RXCTRL(_) 0x03000u
#define IXGBE_REG_RXCTRL_RXEN BIT(0)

// Section 8.2.3.8.6 Receive Descriptor Control
#define IXGBE_REG_RXDCTL(n) ((n) <= 63u ? (0x01028u + 0x40u*(n)) : (0x0D028u + 0x40u*((n)-64u)))
#define IXGBE_REG_RXDCTL_ENABLE BIT(25)

// Section 8.2.3.8.9 Receive Packet Buffer Size
#define IXGBE_REG_RXPBSIZE(n) (0x03C00u + 4u*(n))

// Section 8.2.3.12.5 Security Rx Control
#define IXGBE_REG_SECRXCTRL(_) 0x08D00u
#define IXGBE_REG_SECRXCTRL_RX_DIS BIT(1)

// Section 8.2.3.12.6 Security Rx Status
#define IXGBE_REG_SECRXSTAT(_) 0x08D04u
#define IXGBE_REG_SECRXSTAT_SECRX_RDY BIT(0)

// Section 8.2.3.8.7 Split Receive Control Registers
#define IXGBE_REG_SRRCTL(n) ((n) <= 63u ? (0x01014u + 0x40u*(n)) : (0x0D014u + 0x40u*((n)-64u)))
#define IXGBE_REG_SRRCTL_BSIZEPACKET BITS(0,4)
#define IXGBE_REG_SRRCTL_DROP_EN BIT(28)

// Section 8.2.3.1.2 Device Status Register
#define IXGBE_REG_STATUS(_) 0x00008u
#define IXGBE_REG_STATUS_PCIE_MASTER_ENABLE_STATUS BIT(19)

// Section 8.2.3.9.6 Transmit Descriptor Base Address High
#define IXGBE_REG_TDBAH(n) (0x06004u + 0x40u*(n))

// Section 8.2.3.9.5 Transmit Descriptor Base Address Low
#define IXGBE_REG_TDBAL(n) (0x06000u + 0x40u*(n))

// Section 8.2.3.9.7 Transmit Descriptor Length
#define IXGBE_REG_TDLEN(n) (0x06008u + 0x40u*(n))

// Section 8.2.3.9.9 Transmit Descriptor Tail
#define IXGBE_REG_TDT(n) (0x06018u + 0x40u*(n))

// Section 8.2.3.9.11 Tx Descriptor Completion Write Back Address High
#define IXGBE_REG_TDWBAH(n) (0x0603Cu + 0x40u*(n))

// Section 8.2.3.9.11 Tx Descriptor Completion Write Back Address Low
#define IXGBE_REG_TDWBAL(n) (0x06038u + 0x40u*(n))

// Section 8.2.3.9.10 Transmit Descriptor Control
#define IXGBE_REG_TXDCTL(n) (0x06028u + 0x40u*(n))
#define IXGBE_REG_TXDCTL_PTHRESH BITS(0,6)
#define IXGBE_REG_TXDCTL_HTHRESH BITS(8,14)
#define IXGBE_REG_TXDCTL_ENABLE BIT(25)

// Section 8.2.3.9.13 Transmit Packet Buffer Size
#define IXGBE_REG_TXPBSIZE(n) (0x0CC00u + 4u*(n))

// Section 8.2.3.9.16 Tx Packet Buffer Threshold
#define IXGBE_REG_TXPBTHRESH(n) (0x04950u + 4u*(n))
#define IXGBE_REG_TXPBTHRESH_THRESH BITS(0,9)


// ======
// Device
// ======

struct tn_net_device
{
	uintptr_t addr;
	struct tn_pci_device pci;
};

// ---------------------------------------------------------
// Section 4.6.7.1.2 [Dynamic] Disabling [of Receive Queues]
// ---------------------------------------------------------

static bool ixgbe_recv_disable(const struct tn_net_device* const device, const uint8_t queue)
{
	// "Disable the queue by clearing the RXDCTL.ENABLE bit."
	IXGBE_REG_CLEAR(device->addr, RXDCTL, queue, ENABLE);

	// "The 82599 clears the RXDCTL.ENABLE bit only after all pending memory accesses to the descriptor ring are done.
	//  The driver should poll this bit before releasing the memory allocated to this queue."
	// INTERPRETATION: There is no mention of what to do if the 82599 never clears the bit; 1s seems like a decent timeout
	IF_AFTER_TIMEOUT(1000 * 1000, !IXGBE_REG_CLEARED(device->addr, RXDCTL, queue, ENABLE)) {
		TN_DEBUG("RXDCTL.ENABLE did not clear, cannot disable receive");
		return false;
	}

	// "Once the RXDCTL.ENABLE bit is cleared the driver should wait additional amount of time (~100 us) before releasing the memory allocated to this queue."
	tn_sleep_us(100);

	return true;
}

// --------------------------------
// Section 5.2.5.3.2 Master Disable
// --------------------------------

// See quotes inside to understand the meaning of the return value
static bool ixgbe_device_master_disable(const struct tn_net_device* const device)
{
	// "The device driver disables any reception to the Rx queues as described in Section 4.6.7.1"
	for (uint8_t queue = 0; queue <= IXGBE_RECEIVE_QUEUES_COUNT; queue++) {
		if (!ixgbe_recv_disable(device, queue)) {
			return false;
		}
	}

	// "Then the device driver sets the PCIe Master Disable bit [in the Device Status register] when notified of a pending master disable (or D3 entry)."
	IXGBE_REG_SET(device->addr, CTRL, _, MASTER_DISABLE);

	// "The 82599 then blocks new requests and proceeds to issue any pending requests by this function.
	//  The driver then reads the change made to the PCIe Master Disable bit and then polls the PCIe Master Enable Status bit.
	//  Once the bit is cleared, it is guaranteed that no requests are pending from this function."
	// INTERPRETATION: The next sentence refers to "a given time"; let's say 1 second should be plenty...
	// "The driver might time out if the PCIe Master Enable Status bit is not cleared within a given time."
	IF_AFTER_TIMEOUT(1000 * 1000, !IXGBE_REG_CLEARED(device->addr, STATUS, _, PCIE_MASTER_ENABLE_STATUS)) {
		// "In these cases, the driver should check that the Transaction Pending bit (bit 5) in the Device Status register in the PCI config space is clear before proceeding.
		//  In such cases the driver might need to initiate two consecutive software resets with a larger delay than 1 us between the two of them."
		if (!IXGBE_PCIREG_CLEARED(device->pci, DEVICESTATUS, TRANSACTIONPENDING)) {
			TN_DEBUG("DEVICESTATUS.TRANSACTIONPENDING did not clear, cannot perform master disable");
			return false;
		}

		// "In the above situation, the data path must be flushed before the software resets the 82599.
		//  The recommended method to flush the transmit data path is as follows:"
		// "- Inhibit data transmission by setting the HLREG0.LPBK bit and clearing the RXCTRL.RXEN bit.
		//    This configuration avoids transmission even if flow control or link down events are resumed."
		IXGBE_REG_SET(device->addr, HLREG0, _, LPBK);
		IXGBE_REG_CLEAR(device->addr, RXCTRL, _, RXEN);

		// "- Set the GCR_EXT.Buffers_Clear_Func bit for 20 microseconds to flush internal buffers."
		IXGBE_REG_SET(device->addr, GCREXT, _, BUFFERS_CLEAR_FUNC);
		tn_sleep_us(20);

		// "- Clear the HLREG0.LPBK bit and the GCR_EXT.Buffers_Clear_Func"
		IXGBE_REG_CLEAR(device->addr, HLREG0, _, LPBK);
		IXGBE_REG_CLEAR(device->addr, GCREXT, _, BUFFERS_CLEAR_FUNC);

		// "- It is now safe to issue a software reset."
	}

	return true;
}

// --------------------------
// Section 4.2.1.7 Link Reset
// --------------------------

// INTERPRETATION: The spec has a circular dependency here - resets need master disable, but master disable asks for two resets if it fails!
//                 Assume that if the master disable fails, the resets do not need to go through the master disable step.

static void ixgbe_device_reset(const struct tn_net_device* const device)
{
	// "Prior to issuing link reset, the driver needs to execute the master disable algorithm as defined in Section 5.2.5.3.2."
	bool master_disabled = ixgbe_device_master_disable(device);

	// "Initiated by writing the Link Reset bit of the Device Control register (CTRL.LRST)."
	IXGBE_REG_SET(device->addr, CTRL, _, LRST);

	// See quotes in ixgbe_device_master_disable
	if (master_disabled) {
		tn_sleep_us(2);
		IXGBE_REG_SET(device->addr, CTRL, _, LRST);
	}

	// Section 8.2.3.1.1 Device Control Register
	// "To ensure that a global device reset has fully completed and that the 82599 responds to subsequent accesses,
	//  programmers must wait approximately 1 ms after setting before attempting to check if the bit has cleared or to access (read or write) any other device register."
	// INTERPRETATION: It's OK to access the CTRL register itself to double-reset it as above without waiting a full second,
	//                 and thus this does not contradict the "at least 1 us" rule of the double-reset.
	tn_sleep_us(1000);
}

// -------------------------------------
// Section 4.6.3 Initialization Sequence
// -------------------------------------

static void ixgbe_device_disable_interrupts(const struct tn_net_device* const device)
{
	for (uint8_t n = 0; n < IXGBE_INTERRUPT_REGISTERS_COUNT; n++) {
		// Section 8.2.3.5.4 Extended Interrupt Mask Clear Register (EIMC):
		// "Writing a 1b to any bit clears its corresponding bit in the EIMS register disabling the corresponding interrupt in the EICR register. Writing 0b has no impact"
		IXGBE_REG_SET(device->addr, EIMC, n, MASK);
	}
}

bool tn_net_device_init(const struct tn_pci_device pci_device, struct tn_net_device** out_device)
{
	// The NIC supports 64-bit addresses, so pointers can't be larger
	static_assert(UINTPTR_MAX <= UINT64_MAX, "uintptr_t must fit in an uint64_t");

	// First make sure the PCI device is really an 82599ES 10-Gigabit SFI/SFP+ Network Connection
	// According to https://cateee.net/lkddb/web-lkddb/IXGBE.html, this means vendor ID (bottom 16 bits) 8086, device ID (top 16 bits) 10FB
	uint32_t pci_id = IXGBE_PCIREG_READ(pci_device, ID);
	if (pci_id != ((0x10FBu << 16) | 0x8086u)) {
		TN_DEBUG("PCI device is not what was expected");
		return false;
	}

	// By assumption PCIBRIDGES, the bridges will not get in our way
	// By assumption PCIBARS, the PCI BARs have been configured already
	// Section 9.3.7.1.4 Power Management Control / Status Register (PMCSR):
	// "No_Soft_Reset. This bit is always set to 0b to indicate that the 82599 performs an internal reset upon transitioning from D3hot to D0 via software control of the PowerState bits."
	// Thus, the device cannot go from D3 to D0 without resetting, which would mean losing the BARs.
	if (!IXGBE_PCIREG_CLEARED(pci_device, PMCSR, POWER_STATE)) {
		TN_DEBUG("PCI device not in D0.");
		return false;
	}
	// The bus master may not be ebabled; enable it just in case.
	IXGBE_PCIREG_SET(pci_device, COMMAND, BUS_MASTER_ENABLE);
	// Same for memory reads, i.e. actually using the BARs.
	IXGBE_PCIREG_SET(pci_device, COMMAND, MEMORY_ACCESS_ENABLE);
	// Finally, since we don't want interrupts and certainly not legacy ones, make sure they're disabled
	IXGBE_PCIREG_SET(pci_device, COMMAND, INTERRUPT_DISABLE);

	// Section 8.2.2 Registers Summary PF — BAR 0: As the section title indicate, registers are at the address pointed to by BAR 0, which is the only one we care about
	uint32_t pci_bar0low = IXGBE_PCIREG_READ(pci_device, BAR0_LOW);
	// Sanity check: a 64-bit BAR must have bit 2 of low as 1 and bit 1 of low as 0 as per Table 9-4 Base Address Registers' Fields
	if ((pci_bar0low & BIT(2)) == 0 || (pci_bar0low & BIT(1)) != 0) {
		TN_DEBUG("BAR0 is not a 64-bit BAR");
		return false;
	}
	uint32_t pci_bar0high = IXGBE_PCIREG_READ(pci_device, BAR0_HIGH);
	// No need to detect the size, since we know exactly which device we're dealing with. (This also means no writes to BARs, one less chance to mess everything up)

	struct tn_net_device device;
	device.pci = pci_device;
	// Section 9.3.6.1 Memory and IO Base Address Registers:
	// As indicated in Table 9-4, the low 4 bits are read-only and not part of the address
	uintptr_t dev_phys_addr = (((uint64_t) pci_bar0high) << 32) | (pci_bar0low & ~BITS(0,3));
	// Section 8.1 Address Regions: "Region Size" of "Internal registers memories and Flash (memory BAR)" is "128 KB + Flash_Size"
	// Thus we can ask for 128KB, since we don't know the flash size (and don't need it thus no need to actually check it)
	if (!tn_mem_phys_to_virt(dev_phys_addr, 128 * 1024, &device.addr)) {
		return false;
	}
	TN_INFO("Device %02"PRIx8":%02"PRIx8".%"PRIx8" mapped to 0x%016"PRIxPTR, device.pci.bus, device.pci.device, device.pci.function, device.addr);

	// "The following sequence of commands is typically issued to the device by the software device driver in order to initialize the 82599 for normal operation.
	//  The major initialization steps are:"
	// "- Disable interrupts"
	// "- Issue global reset and perform general configuration (see Section 4.6.3.2)."
	// 	Section 4.6.3.1 Interrupts During Initialization:
	// 	"Most drivers disable interrupts during initialization to prevent re-entrance.
	// 	 Interrupts are disabled by writing to the EIMC registers.
	//	 Note that the interrupts also need to be disabled after issuing a global reset, so a typical driver initialization flow is:
	//	 1. Disable interrupts.
	//	 2. Issue a global reset.
	//	 3. Disable interrupts (again)."
	// 	Section 4.6.3.2 Global Reset and General Configuration:
	//	"Device initialization typically starts with a software reset that puts the device into a known state and enables the device driver to continue the initialization sequence.
	//	 Following a Global Reset the Software driver should wait at least 10msec to enable smooth initialization flow."
	ixgbe_device_disable_interrupts(&device);
	ixgbe_device_reset(&device);
	tn_sleep_us(10 * 1000);
	ixgbe_device_disable_interrupts(&device);
	//	"To enable flow control, program the FCTTV, FCRTL, FCRTH, FCRTV and FCCFG registers.
	//	 If flow control is not enabled, these registers should be written with 0x0.
	//	 If Tx flow control is enabled then Tx CRC by hardware should be enabled as well (HLREG0.TXCRCEN = 1b).
	//	 Refer to Section 3.7.7.3.2 through Section 3.7.7.3.5 for the recommended setting of the Rx packet buffer sizes and flow control thresholds.
	//	 Note that FCRTH[n].RTH fields must be set by default regardless if flow control is enabled or not.
	//	 Typically, FCRTH[n] default value should be equal to RXPBSIZE[n]-0x6000. FCRTH[n].
	//	 FCEN should be set to 0b if flow control is not enabled as all the other registers previously indicated."
	// INTERPRETATION: Sections 3.7.7.3.{2-5} are irrelevant here since we do not want flow control.
	// Section 8.2.3.3.2 Flow Control Transmit Timer Value n (FCTTVn): all init vals are 0
	// INTERPRETATION: We do not need to set FCTTV
	// Section 8.2.3.3.3 Flow Control Receive Threshold Low (FCRTL[n]): all init vals are 0
	// INTERPRETATION: We do not need to set FCRTL
	// Section 8.2.3.3.5 Flow Control Refresh Threshold Value (FCRTV): all init vals are 0
	// INTERPRETATION: We do not need to set FCRTV
	// Section 8.2.3.3.7 Flow Control Configuration (FCCFG): all init vals are 0
	// INTERPRETATION: We do not need to set FCCFG
	// Section 8.2.3.8.9 Receive Packet Buffer Size (RXPBSIZE[n])
	//	"The size is defined in KB units and the default size of PB[0] is 512 KB."
	// Section 8.2.3.3.4 Flow Control Receive Threshold High (FCRTH[n])
	//	"Bits 4:0; Reserved"
	//	"RTH: Bits 18:5; Receive packet buffer n FIFO high water mark for flow control transmission (32 bytes granularity)."
	//	"Bits 30:19; Reserved"
	//	"FCEN: Bit 31; Transmit flow control enable for packet buffer n."
	//	"This register contains the receive threshold used to determine when to send an XOFF packet and counts in units of bytes.
	//	 This value must be at least eight bytes less than the maximum number of bytes allocated to the receive packet buffer and the lower four bits must be programmed to 0x0 (16-byte granularity).
	//	 Each time the receive FIFO reaches the fullness indicated by RTH, hardware transmits a pause frame if the transmission of flow control frames is enabled."
	// INTERPRETATION: There is an obvious contradiction in the stated granularities (16 vs 32 bytes). We assume 32 is correct, since the DPDK ixgbe driver treats it that way..
	// INTERPRETATION: We assume that the "RXPBSIZE[n]-0x6000" calculation above refers to the RXPBSIZE in bytes (otherwise the size of FCRTH[n].RTH would be negative by default...)
	//                 Thus we set FCRTH[0].RTH = 512 * 1024 - 0x6000, which importantly has the 5 lowest bits unset.
	IXGBE_REG_WRITE(device.addr, FCRTH, 0, RTH, 512 * 1024 - 0x6000);
	// "- Wait for EEPROM auto read completion."
	// INTERPRETATION: This refers to Section 8.2.3.2.1 EEPROM/Flash Control Register (EEC), Bit 9 "EEPROM Auto-Read Done"
	// INTERPRETATION: No timeout is mentioned, so we use 1s.
	IF_AFTER_TIMEOUT(1000 * 1000, IXGBE_REG_CLEARED(device.addr, EEC, _, AUTO_RD)) {
		TN_DEBUG("EEPROM auto read timed out");
		return false;
	}
	// INTERPRETATION: We also need to check bit 8 of the same register, "EEPROM Present", which indicates "EEPROM is present and has the correct signature field. This bit is read-only.",
	//                 since bit 9 "is also set when the EEPROM is not present or whether its signature field is not valid."
	// INTERPRETATION: We also need to check whether the EEPROM has a valid checksum, using the FWSM's register EXT_ERR_IND, where "0x00 = No error"
	if (IXGBE_REG_CLEARED(device.addr, EEC, _, EE_PRES) || !IXGBE_REG_CLEARED(device.addr, FWSM, _, EXT_ERR_IND)) {
		TN_DEBUG("EEPROM not present or invalid");
		return false;
	}
	// "- Wait for DMA initialization done (RDRXCTL.DMAIDONE)."
	// INTERPRETATION: Once again, no timeout mentioned, so we use 1s
	IF_AFTER_TIMEOUT(1000 * 1000, IXGBE_REG_CLEARED(device.addr, RDRXCTL, _, DMAIDONE)) {
		TN_DEBUG("DMA init timed out");
		return false;
	}
	// "- Setup the PHY and the link (see Section 4.6.4)."
	//	Section 8.2.3.22.19 Auto Negotiation Control Register (AUTOC):
	//	"Also programmable via EEPROM." is applied to all fields except bit 0, "Force Link Up"
	// INTERPRETATION: AUTOC is already programmed via the EEPROM, we do not need to set up the PHY/link.
	// "- Initialize all statistical counters (see Section 4.6.5)."
	// By assumption NOCARE, we don't need to do anything here.
	// "- Initialize receive (see Section 4.6.7)."
	// Section 4.6.7 Receive Initialization
	//	"Initialize the following register tables before receive and transmit is enabled:"
	//	"- Receive Address (RAL[n] and RAH[n]) for used addresses."
	//	"Program the Receive Address register(s) (RAL[n], RAH[n]) per the station address
	//	 This can come from the EEPROM or from any other means (for example, it could be stored anywhere in the EEPROM or even in the platform PROM for LOM design)."
	//	Section 8.2.3.7.9 Receive Address High (RAH[n]):
	//		"After reset, if the EEPROM is present, the first register (Receive Address Register 0) is loaded from the IA field in the EEPROM, its Address Select field is 00b, and its Address Valid field is 1b."
	// INTERPRETATION: Since we checked that the EEPROM is present and valid, RAH[0] and RAL[0] are initialized from the EEPROM, thus we do not need to initialize them.
	//	"- Receive Address High (RAH[n].VAL = 0b) for unused addresses."
	//	Section 8.2.3.7.9 Receive Address High (RAH[n]):
	//		"After reset, if the EEPROM is present, the first register (Receive Address Register 0) is loaded [...] The Address Valid field for all of the other registers are 0b."
	// INTERPRETATION: Since we checked that the EEPROM is present and valid, RAH[n] for n != 0 has Address Valid == 0, thus we do not need to initialize them.
	//	"- Unicast Table Array (PFUTA)."
	//	Section 8.2.3.27.12 PF Unicast Table Array (PFUTA[n]):
	//		"There is one register per 32 bits of the unicast address table"
	//		"This table should be zeroed by software before start of operation."
	for (uint8_t n = 0; n < IXGBE_UNICAST_TABLE_ARRAY_SIZE / 32; n++) {
		IXGBE_REG_CLEAR(device.addr, PFUTA, n);
	}
	//	"- VLAN Filter Table Array (VFTA[n])."
	//	Section 7.1.1.2 VLAN Filtering:
	//		Figure 7-3 states that matching to a valid VFTA[n] is only done if VLNCTRL.VFE is set.
	//	Section 8.2.3.7.2 VLAN Control Register (VLNCTRL):
	//		"VFE: Bit 30; Init val: 0; VLAN Filter Enable. 0b = Disabled (filter table does not decide packet acceptance)"
	// INTERPRETATION: By default, VLAN filtering is disabled, and the value of VFTA[n] does not matter; thus we do not need to initialize them.
	//	"- VLAN Pool Filter (PFVLVF[n])."
	// INTERPRETATION: While the spec appears to mention PFVLVF only in conjunction with VLNCTRL.VFE being enabled, let's be conservative and initialize them anyway.
	// 	Section 8.2.3.27.15 PF VM VLAN Pool Filter (PFVLVF[n]):
	//		"Software should initialize these registers before transmit and receive are enabled."
	for (uint8_t n = 0; n < IXGBE_VLAN_FILTER_COUNT; n++) {
		IXGBE_REG_CLEAR(device.addr, PFVLVF, n);
	}
	//	"- MAC Pool Select Array (MPSAR[n])."
	//	Section 8.2.3.7.10 MAC Pool Select Array (MPSAR[n]):
	//		"Software should initialize these registers before transmit and receive are enabled."
	//		"Each couple of registers '2*n' and '2*n+1' are associated with Ethernet MAC address filter 'n' as defined by RAL[n] and RAH[n].
	//		 Each bit when set, enables packet reception with the associated pools as follows:
	//		 Bit 'i' in register '2*n' is associated with POOL 'i'.
	//		 Bit 'i' in register '2*n+1' is associated with POOL '32+i'."
	// INTERPRETATION: We should enable all pools with address 0, just in case, and disable everything else since we only have 1 MAC address.
	IXGBE_REG_WRITE(device.addr, MPSAR, 0, 0xFFFFFFFFu);
	IXGBE_REG_WRITE(device.addr, MPSAR, 1, 0xFFFFFFFFu);
	for (uint16_t n = 2; n < IXGBE_RECEIVE_ADDRS_COUNT * 2; n++) {
		IXGBE_REG_CLEAR(device.addr, MPSAR, n);
	}
	//	"- VLAN Pool Filter Bitmap (PFVLVFB[n])."
	// INTERPRETATION: See above remark on PFVLVF
	//	Section 8.2.3.27.16: PF VM VLAN Pool Filter Bitmap
	for (uint8_t n = 0; n < IXGBE_VLAN_FILTER_COUNT * 2; n++) {
		IXGBE_REG_CLEAR(device.addr, PFVLVFB, n);
	}
	//	"Set up the Multicast Table Array (MTA) registers.
	//	 This entire table should be zeroed and only the desired multicast addresses should be permitted (by writing 0x1 to the corresponding bit location).
	//	 Set the MCSTCTRL.MFE bit if multicast filtering is required."
	// Section 8.2.3.7.7 Multicast Table Array (MTA[n]): "Word wide bit vector specifying 32 bits in the multicast address filter table."
	for (uint8_t n = 0; n < IXGBE_MULTICAST_TABLE_ARRAY_SIZE / 32; n++) {
		IXGBE_REG_CLEAR(device.addr, MTA, n);
	}
	//	"Initialize the flexible filters 0...5 — Flexible Host Filter Table registers (FHFT)."
	//	Section 5.3.3.2 Flexible Filter:
	//		"The 82599 supports a total of six host flexible filters.
	//		 Each filter can be configured to recognize any arbitrary pattern within the first 128 bytes of the packet.
	//		 To configure the flexible filter, software programs the required values into the Flexible Host Filter Table (FHFT).
	//		 These contain separate values for each filter.
	//		 Software must also enable the filter in the WUFC register, and enable the overall wake-up functionality must be enabled by setting the PME_En bit in the PMCSR or the WUC register."
	//	Section 8.2.3.24.2 Wake Up Filter Control Register (WUFC):
	//		"FLX{0-5}: Bits {16-21}; Init val 0b; Flexible Filter {0-5} Enable"
	// INTERPRETATION: Since WUFC.FLX{0-5} are disabled by default, and FHFT(n) only matters if the corresponding WUFC.FLX is enabled, we do not need to do anything by assumption NOWANT
	//	"After all memories in the filter units previously indicated are initialized, enable ECC reporting by setting the RXFECCERR0.ECCFLT_EN bit."
	//	Section 8.2.3.7.23 Rx Filter ECC Err Insertion 0 (RXFECCERR0):
	//		"Filter ECC Error indication Enablement.
	//		 When set to 1b, enables the ECC-INT + the RXF-blocking during ECC-ERR in one of the Rx filter memories.
	//		 At 0b, the ECC logic can still function overcoming only single errors while dual or multiple errors can be ignored silently."
	// INTERPRETATION: Since we do not want flexible filters, this step is not necessary.
	//	"Program the different Rx filters and Rx offloads via registers FCTRL, VLNCTRL, MCSTCTRL, RXCSUM, RQTC, RFCTL, MPSAR, RSSRK, RETA, SAQF, DAQF, SDPQF, FTQF, SYNQF, ETQF, ETQS, RDRXCTL, RSCDBU."
	// We do not touch FCTRL here, if the user wants promiscuous mode they will call the appropriate function.
	//	Section 8.2.3.7.2 VLAN Control Register (VLNCTRL):
	//		"Bit 30, VLAN Filter Enable, Init val 0b; 0b = Disabled."
	// We do not need to set up VLNCTRL by assumption NOWANT
	//	Section 8.2.3.7.2 Multicast Control Register (MCSTCTRL):
	//		"Bit 2, Multicast Filter Enable, Init val 0b; 0b = The multicast table array filter (MTA[n]) is disabled."
	// Since multicast filtering is disabled by default, we do not need to set up MCSTCTRL by assumption NOWANT
	// 	Section 8.2.3.7.5 Receive Checksum Control (RXCSUM):
	//		"Bit 12, Init val 0b, IP Payload Checksum Enable."
	//		"Bit 13, Init val 0b, RSS/Fragment Checksum Status Selection."
	//		"The Receive Checksum Control register controls the receive checksum offloading features of the 82599."
	// Since checksum offloading is disabled by default, we do not need to set up RXCSUM by assumption NOWANT
	//	Section 8.2.3.7.13 RSS Queues Per Traffic Class Register (RQTC):
	//		"RQTC{0-7}, This field is used only if MRQC.MRQE equals 0100b or 0101b."
	//	Section 8.2.3.7.12 Multiple Receive Queues Command Register (MRQC):
	//		"MRQE, Init val 0x0"
	// Since RSS is disabled by default, we do not need to do anything by assumption NOWANT
	//	Section 8.2.3.7.6 Receive Filter Control Register (RFCTL):
	//		"Bit 5, Init val 0b; RSC Disable. The default value is 0b (RSC feature is enabled)."
	//		"Bit 6, Init val 0b; NFS Write disable."
	//		"Bit 7, Init val 0b; NFS Read disable."
	// We do not need to write to RFCTL by assumption NOWANT
	// We already initialized MPSAR earlier.
	//	Section 4.6.10.1.1 Global Filtering and Offload Capabilities:
	//		"In RSS mode, the RSS key (RSSRK) and redirection table (RETA) should be programmed."
	// Since we do not want RSS, we do not need to touch RSSRK or RETA.
	//	Section 8.2.3.7.19 Five tuple Queue Filter (FTQF[n]):
	//		All bits have an unspecified default value.
	//		"Mask, bits 29:25: Mask bits for the 5-tuple fields (1b = don’t compare)."
	//		"Queue Enable, bit 31; When set, enables filtering of Rx packets by the 5-tuple defined in this filter to the queue indicated in register L34TIMIR."
	// We clear Queue Enable. We then do not need to deal with SAQF, DAQF, SDPQF, SYNQF, by assumption NOWANT
	for (uint8_t n = 0; n < IXGBE_5TUPLE_FILTERS_COUNT; n++) {
		IXGBE_REG_CLEAR(device.addr, FTQF, n, QUEUE_ENABLE);
	}
	//	Section 7.1.2.3 L2 Ethertype Filters:
	//		"The following flow is used by the Ethertype filters:
	//		 1. If the Filter Enable bit is cleared, the filter is disabled and the following steps are ignored."
	//	Section 8.2.3.7.21 EType Queue Filter (ETQF[n]):
	//		"Bit 31, Filter Enable, Init val 0b; 0b = The filter is disabled for any functionality."
	// Since filters are disabled by default, we do not need to do anything to ETQF and ETQS by assumption NOWANT
	//	Section 8.2.3.8.8 Receive DMA Control Register (RDRXCTL):
	//		The only non-reserved, non-RO bit is "CRCStrip, bit 1, Init val 0; 0b = No CRC Strip by HW."
	// By assumption CRC, we enable CRCStrip
	IXGBE_REG_SET(device.addr, RDRXCTL, _, CRC_STRIP);
	//	"Note that RDRXCTL.CRCStrip and HLREG0.RXCRCSTRP must be set to the same value. At the same time the RDRXCTL.RSCFRSTSIZE should be set to 0x0 as opposed to its hardware default."
	// As explained earlier, HLREG0.RXCRCSTRP is already set to 1, so that's fine
	IXGBE_REG_CLEAR(device.addr, RDRXCTL, _, RSCFRSTSIZE);
	//	Section 8.2.3.8.8 Receive DMA Control Register (RDRXCTL):
	//		"RSCACKC [...] Software should set this bit to 1b."
	IXGBE_REG_SET(device.addr, RDRXCTL, _, RSCACKC);
	//		"FCOE_WRFIX [...] Software should set this bit to 1b."
	IXGBE_REG_SET(device.addr, RDRXCTL, _, FCOE_WRFIX);
	// INTERPRETATION: The setup forgets to mention RSCACKC and FCOE_WRFIX, but we should set those properly anyway.
	//	Section 8.2.3.8.12 RSC Data Buffer Control Register (RSCDBU):
	//		The only non-reserved bit is "RSCACKDIS, bit 7, init val 0b; Disable RSC for ACK Packets."
	// We do not need to change RSCDBU by assumption NOCARE
	//	"Program RXPBSIZE, MRQC, PFQDE, RTRUP2TC, MFLCN.RPFCE, and MFLCN.RFCE according to the DCB and virtualization modes (see Section 4.6.11.3)."
	//	Section 4.6.11.3.4 DCB-Off, VT-Off:
	//		"Set the configuration bits as specified in Section 4.6.11.3.1 with the following exceptions:"
	//		"Disable multiple packet buffers and allocate all queues and traffic to PB0:"
	//		"- RXPBSIZE[0].SIZE=0x200, RXPBSIZE[1-7].SIZE=0x0"
	//		Section 8.2.3.8.9 Receive Packet Buffer Size (RXPBSIZE[n]):
	//			"SIZE, Init val 0x200"
	//			"The default size of PB[1-7] is also 512 KB but it is meaningless in non-DCB mode."
	// INTERPRETATION: We do not need to change PB[0] but must clear PB[1-7]
	for (uint8_t n = 1; n < IXGBE_TRAFFIC_CLASSES_COUNT; n++) {
		IXGBE_REG_CLEAR(device.addr, RXPBSIZE, n);
	}
	//		"- MRQC"
	//			"- Set MRQE to 0xxxb, with the three least significant bits set according to the RSS mode"
	// 			Section 8.2.3.7.12 Multiple Receive Queues Command Register (MRQC): "MRQE, Init Val 0x0; 0000b = RSS disabled"
	// Thus we do not need to modify MRQC.
	//		(from 4.6.11.3.1) "Queue Drop Enable (PFQDE) — In SR-IO the QDE bit should be set to 1b in the PFQDE register for all queues. In VMDq mode, the QDE bit should be set to 0b for all queues."
	// We do not need to change PFQDE by assumption NOWANT
	//		"- Rx UP to TC (RTRUP2TC), UPnMAP=0b, n=0,...,7"
	//		Section 8.2.3.10.4 DCB Receive User Priority to Traffic Class (RTRUP2TC): All init vals = 0
	// Thus we do not need to modify RTRUP2TC.
	//		"Allow no-drop policy in Rx:"
	//		"- PFQDE: The QDE bit should be set to 0b in the PFQDE register for all queues enabling per queue policy by the SRRCTL[n] setting."
	//		Section 8.2.3.27.9 PF PF Queue Drop Enable Register (PFQDE): "QDE, Init val 0b"
	// Thus we do not need to modify PFQDE.
	//		"Disable PFC and enable legacy flow control:"
	//		"- Disable receive PFC via: MFLCN.RPFCE=0b"
	//		Section 8.2.3.22.34 MAC Flow Control Register (MFLCN): "RPFCE, Init val 0b"
	// Thus we do not need to modify MFLCN.RPFCE.
	//		"- Enable receive legacy flow control via: MFLCN.RFCE=1b"
	IXGBE_REG_SET(device.addr, MFLCN, _, RFCE);
	//	"Enable jumbo reception by setting HLREG0.JUMBOEN in one of the following two cases:
	//	 1. Jumbo packets are expected. Set the MAXFRS.MFS to expected max packet size.
	//	 2. LinkSec encapsulation is expected."
	// We do not have anything to do here by assumption NOWANT
	//	"Enable receive coalescing if required as described in Section 4.6.7.2."
	// We do not need to do anything here by assumption NOWANT

	// We do not set up receive queues at this point.

	// "- Initialize transmit (see Section 4.6.8)."
	//	Section 4.6.8 Transmit Initialization:
	//		"- Program the HLREG0 register according to the required MAC behavior."
	//		Section 8.2.3.22.8 MAC Core Control 0 Register (HLREG0):
	//			"TXCRCEN, Init val 1b; Tx CRC Enable, Enables a CRC to be appended by hardware to a Tx packet if requested by user."
	// By assumption CRC, we want this default behavior.
	//			"RXCRCSTRP [...] Rx CRC STRIP [...] 1b = Strip CRC by HW (Default)."
	// We do not need to change this by assumption CRC
	//			"JUMBOEN [...] Jumbo Frame Enable [...] 0b = Disable jumbo frames (default)."
	// We do not need to change this by assumption NOWANT
	//			"TXPADEN, Init val 1b; Tx Pad Frame Enable. Pad short Tx frames to 64 bytes if requested by user."
	// We do not need to change this by assumption TXPAD
	//			"LPBK, Init val 0b; LOOPBACK. Turn On Loopback Where Transmit Data Is Sent Back Through Receiver."
	// We do not need to change this by assumption NOWANT
	//			"MDCSPD [...] MDC SPEED."
	//			"CONTMDC [...] Continuous MDC"
	// We do not need to change these by assumption TRUST
	//			"PREPEND [...] Number of 32-bit words starting after the preamble and SFD, to exclude from the CRC generator and checker (default – 0x0)."
	// We do not need to change this by assumption CRC
	//			"RXLNGTHERREN, Init val 1b [...] Rx Length Error Reporting. 1b = Enable reporting of rx_length_err events"
	// We do not need to change this by assumption REPORT
	//			"RXPADSTRIPEN [...] 0b = [...] (default). 1b = [...] (debug only)."
	// We do not need to change this by assumption NOWANT
	//		"- Program TCP segmentation parameters via registers DMATXCTL (while maintaining TE bit cleared), DTXTCPFLGL, and DTXTCPFLGH; and DCA parameters via DCA_TXCTRL."
	//			Section 8.2.3.9.2 DMA Tx Control (DMATXCTL):
	//				There is only one field besides TE that the user should modify: "GDV, Init val 0b, Global Double VLAN Mode."
	// We do not need to change DMATXCTL for now by assumption NOWANT
	//			Section 8.2.3.9.3 DMA Tx TCP Flags Control Low (DTXTCPFLGL),
	//			Section 8.2.3.9.4 DMA Tx TCP Flags Control High (DTXTCPFLGH):
	//				"This register holds the mask bits for the TCP flags in Tx segmentation."
	// We do not need to modify DTXTCPFLGL/H by assumption TRUST.
	// Now for DCA_TXCTRL...
	// First, let's make sure we actually can use DCA
	// TODO bench with/without DCA/DCA_with_payload
	if (!tn_dca_is_enabled()) {
		TN_DEBUG("DCA is disabled");
		return false;
	}
	// INTERPRETATION: Since register DCA_CTRL exists and disables DCA by default, we need to change it as well.
	// "DCA Disable. Init val 1b. [...] 0b = DCA tagging is enabled for this device."
	IXGBE_REG_CLEAR(device.addr, DCACTRL, _, DCA_DIS);
	// "DCA Mode. Init val 0x0. [...] 0001b = DCA 1.0 is supported"
	IXGBE_REG_WRITE(device.addr, DCACTRL, _, DCA_MODE, 1);
	uint8_t dca_id = tn_dca_get_id();
	for (uint8_t n = 0; n < IXGBE_SEND_QUEUES_COUNT; n++) {
		//		Section 8.2.3.11.2 Tx DCA Control Registers (DCA_TXCTRL[n]):
		//			"Tx Descriptor DCA EN, Init val 0b; Descriptor DCA Enable. When set, hardware enables DCA for all Tx descriptors written back into memory.
		//			 When cleared, hardware does not enable DCA for descriptor write-backs. This bit is cleared as a default and also applies to head write back when enabled."
		IXGBE_REG_SET(device.addr, DCATXCTRL, n, TX_DESCRIPTOR_DCA_EN);
		//			"CPUID [...] DCA 1.0 capable platforms — the device driver programs a value, based on the relevant APIC ID, associated with this Tx queue."
		IXGBE_REG_WRITE(device.addr, DCATXCTRL, n, CPUID, (uint32_t) dca_id);
	}
	// INTEPRETATION: We should set DCA_RXCTRL as well (it's not mentioned anywhere to be part of the setup, but since we just did DCA_TXCTRL...)
	for (uint8_t n = 0; n < IXGBE_RECEIVE_QUEUES_COUNT; n++) {
		//		Section 8.2.3.11.1 Rx DCA Control Register (DCA_RXCTRL[n]):
		//			"Descriptor DCA EN. When set, hardware enables DCA for all Rx descriptors written back into memory. [...]"
		IXGBE_REG_SET(device.addr, DCARXCTRL, n, RX_DESCRIPTOR_DCA_EN);
		IXGBE_REG_SET(device.addr, DCARXCTRL, n, RX_PAYLOAD_DCA_EN);
		//			"CPUID [...] DCA 1.0 capable platforms — The device driver programs a value, based on the relevant APIC ID, associated with this Rx queue."
		IXGBE_REG_WRITE(device.addr, DCARXCTRL, n, CPUID, (uint32_t) dca_id);
	}
	//				There are fields dealing with relaxed ordering; Section 3.1.4.5.3 Relaxed Ordering states that it "enables the system to optimize performance", with no apparent correctness impact.
	// INTERPRETATION: Relaxed ordering has no correctness issues, and thus should be enabled.
	//		"- Set RTTDCS.ARBDIS to 1b."
	IXGBE_REG_SET(device.addr, RTTDCS, _, ARBDIS);
	//		"- Program DTXMXSZRQ, TXPBSIZE, TXPBTHRESH, MTQC, and MNGTXMAP, according to the DCB and virtualization modes (see Section 4.6.11.3)."
	//		Section 4.6.11.3.4 DCB-Off, VT-Off:
	//			"Set the configuration bits as specified in Section 4.6.11.3.1 with the following exceptions:"
	//			"- TXPBSIZE[0].SIZE=0xA0, TXPBSIZE[1-7].SIZE=0x0"
	//			Section 8.2.3.9.13 Transmit Packet Buffer Size (TXPBSIZE[n]):
	//				"SIZE, Init val 0xA0"
	//				"At default setting (no DCB) only packet buffer 0 is enabled and TXPBSIZE values for TC 1-7 are meaningless."
	// INTERPRETATION: We do not need to change TXPBSIZE[0]. Let's stay on the safe side and clear TXPBSIZE[1-7] anyway.
	for (uint8_t n = 1; n < IXGBE_TRAFFIC_CLASSES_COUNT; n++) {
		IXGBE_REG_CLEAR(device.addr, TXPBSIZE, n);
	}
	//			"- TXPBTHRESH.THRESH[0]=0xA0 — Maximum expected Tx packet length in this TC TXPBTHRESH.THRESH[1-7]=0x0"
	// INTERPRETATION: Typo in the spec; should be TXPBTHRESH[0].THRESH
	//			Section 8.2.3.9.16 Tx Packet Buffer Threshold (TXPBTHRESH):
	//				"Default values: 0x96 for TXPBSIZE0, 0x0 for TXPBSIZE1-7."
	// INTERPRETATION: Typo in the spec, this refers to TXPBTHRESH, not TXPBSIZE.
	// Thus we need to set TXPBTHRESH[0] but not TXPBTHRESH[1-7].
	IXGBE_REG_WRITE(device.addr, TXPBTHRESH, 0, THRESH, 0xA0u - IXGBE_PACKET_SIZE_MAX);
	//		"- MTQC"
	//			"- Clear both RT_Ena and VT_Ena bits in the MTQC register."
	//			"- Set MTQC.NUM_TC_OR_Q to 00b."
	//			Section 8.2.3.9.15 Multiple Transmit Queues Command Register (MTQC):
	//				"RT_Ena, Init val 0b"
	//				"VT_Ena, Init val 0b"
	//				"NUM_TC_OR_Q, Init val 00b"
	// Thus we do not need to modify MTQC.
	//		"- DMA TX TCP Maximum Allowed Size Requests (DTXMXSZRQ) — set Max_byte_num_req = 0xFFF = 1 MB"
	IXGBE_REG_WRITE(device.addr, DTXMXSZRQ, _, MAX_BYTES_NUM_REQ, 0xFFF);
	// INTERPRETATION: Section 4.6.11.3 does not refer to MNGTXMAP, but since it's a management-related register we can ignore it here.
	//		"- Clear RTTDCS.ARBDIS to 0b"
	IXGBE_REG_CLEAR(device.addr, RTTDCS, _, ARBDIS);
	// We've already done DCB/VT config earlier, no need to do anything here.
	// "- Enable interrupts (see Section 4.6.3.1)."
	// 	Section 4.6.3.1 Interrupts During Initialization "After initialization completes, a typical driver enables the desired interrupts by writing to the IMS register."
	// We don't need to do anything here by assumption NOWANT

	uintptr_t device_addr;
	if (!tn_mem_allocate(sizeof(struct tn_net_device), &device_addr)) {
		TN_DEBUG("Could not allocate device struct");
		return false;
	}
	struct tn_net_device* heap_device = (struct tn_net_device*) device_addr;
	*heap_device = device;
	*out_device = heap_device;
	return true;
}


// ============================
// Section 7.1.1.1 L2 Filtering
// ============================

bool tn_net_device_set_promiscuous(const struct tn_net_device* const device)
{
	// "A packet passes successfully through L2 Ethernet MAC address filtering if any of the following conditions are met:"
	// 	Section 8.2.3.7.1 Filter Control Register:
	// 	"Before receive filters are updated/modified the RXCTRL.RXEN bit should be set to 0b.
	// 	After the proper filters have been set the RXCTRL.RXEN bit can be set to 1b to re-enable the receiver."
	bool was_rx_enabled = !IXGBE_REG_CLEARED(device->addr, RXCTRL, _, RXEN);
	if (was_rx_enabled) {
		IXGBE_REG_CLEAR(device->addr, RXCTRL, _, RXEN);
	}
	// "Unicast packet filtering — Promiscuous unicast filtering is enabled (FCTRL.UPE=1b) or the packet passes unicast MAC filters (host or manageability)."
	IXGBE_REG_SET(device->addr, FCTRL, _, UPE);
	// "Multicast packet filtering — Promiscuous multicast filtering is enabled by either the host or manageability (FCTRL.MPE=1b or MANC.MCST_PASS_L2 =1b) or the packet matches one of the multicast filters."
	IXGBE_REG_SET(device->addr, FCTRL, _, MPE);
	// "Broadcast packet filtering to host — Promiscuous multicast filtering is enabled (FCTRL.MPE=1b) or Broadcast Accept Mode is enabled (FCTRL.BAM = 1b)."
	// Nothing to do here, since we just enabled MPE

	if (was_rx_enabled) {
		IXGBE_REG_SET(device->addr, RXCTRL, _, RXEN);
	}

	// This function cannot fail (for now?)
	return true;
}


// =================================================================================================================
// PIPES
// =====
// Pipes transfer packets from a receive queue to one or more send queues, optionally modifying the packet.
// The queues need not be on the same device.
// To keep the packet modification customizable, pipe operation is split in three parts:
// receiving a packet, modifying it, and sending it.
// The rest of this description assumes a single send queue; see remarks at the end for supporting more than one.
//
// Hardware background
// -------------------
// The driver and NIC communicate via "descriptors", which are 16-byte structures.
// The first 8 bytes, for both send and receive, are the address of the buffer to use.
// The next 8 bytes are written by the NIC during receive, and read by the NIC during send,
// and contain configuration data.
// In the rest of this text, "packet" implies both a descriptor and its associated buffer.
//
// NIC receiving is dictated by two pointers to a descriptor ring: the receive head and receive tail.
// All packets between the head and the tail are owned by the NIC, i.e. ready to be received.
// Upon packet reception, the NIC increments the head and sets a "descriptor done" bit in the packet metadata..
// All packets between the tail and the head are owned by software, i.e. either given back by the NIC
// because they now contain received data, or not given to the NIC yet and thus unused.
// Software can give packets to the NIC for reception by incrementing the tail.
// NIC sending has the same two-pointers-to-ring structure:
// packets owned by the NIC are ready to be sent, and packets owned by software are sent or unused.
//
// Software checks for received packets not by reading the send head, which could cause data races according to
// the spec, but by scanning for the "descriptor done" bit.
//
//
// Packet states
// -------------
// Each packet in a pipe can be in one of the following states:
// - Sent
// - Sending
// - Processed
// - Received
// - Receiving
//
// By construction, for each state, there is exactly one contiguous block of packets in that state,
// and blocks are ordered in the same order as our state order.
//
//
// Ring
// ----
// The insight behind this implementation is that because of the "contiguous blocks" structure,
// and because packets owned by software are completely hidden from the NIC's point of view,
// the same ring buffer can be used for receiving and sending, with the same packet buffers.
// This implies that some packets which the NIC receive queue believes are owned by software
// are actually owned by the NIC send queue, and vice-versa.
// Thus there is no need to dynamically allocate or free buffers from a pool as e.g. DPDK does.
//
// There are thus seven delimiters in the ring, one per state (including hidden ones).
// Thus the ring looks like this:
//
// ------------------- receive tail
// |      Sent       |
// ------------------- send head
// |     Sending     |
// ------------------- send tail
// |    Processed    |
// ------------------- processed delimiter
// |     Received    |
// ------------------- receive head
// |    Receiving    |
// ------------------- receive tail (same as top - it's a ring!)
//
// The initial state is that all delimiters are set to 0, except the receive tail which is set to the ring size - 1,
// which means all packets are in the "receiving" state.
//
//
// Ring actors
// -----------
// There are conceptually five concurrent actors operating on the ring:
// - The NIC send queue, which increments the send head and thus transitions packets
//   from "sending" to "sent"; its invariant is send_head <= send_tail
// - The pipe sender, which increments the send tail and thus transitions packets
//   from "processed" to "sending"; its invariant is send_tail <= processed_delimiter
// - The pipe processor, which increments the processed delimiter and thus transitions packets
//   from "received" to "processed"; its invariant is processed_delimiter <= receive_head
// - The NIC receive queue, which increments the receive head and thus transitions packets
//   from "receiving" to "received"; its invariant is receive_head <= receive_tail
// - The pipe bookkeeper, which increments the receive tail and thus transitions packets
//   from "sent" to "receiving"; its invariant is receive_tail <= send_head
// All actors can be modeled as operating on one packet at a time, even if they internally choose to operate
// on multiple packets at a time, as long as their multi-packet operations looks like multiple single-packet
// operations from the point of view of other actors.
// Pipe liveness requires that at least one actor must be active at any given time.
//
//
// Pipe design
// -----------
// The following constraints to the pipe simplify both programming and formal reasoning:
// - The pipe processing actor must operate on a single packet at a time
// - All pipe actors are executed sequentially, i.e. without parallelism
//
// The API allows the developer to specify where to receive and send, and to run the pipe given a function
// that is called for every packet and may modify the packet.
//
//
// Pipe implementation
// -------------------
// This implementation uses a very basic scheduling algorithm between the actors: schedule the processor N times
// in a row, then schedule the bookkeeper once and the sender once.
// When scheduled, the processor only performs work if processed_delimiter < receive_head, to not violate its invariant;
// as mentioned earlier, this is actually performed using "descriptor done" bits to avoid a data race.
// The bookkeeper and sender always mirror send_head to receive_tail and processed_delimiter to send_tail respectively.
//
//
// Performance considerations
// --------------------------
// It is important to remember that all NIC registers are in memory-mapped PCIe space,
// which implies that loads and stores cannot be cached and are much slower than standard memory.
//
// This means that reading the send head to mirror it to the receive tail is expensive; thankfully, the NIC
// has a feature called "TX head pointer write back": we tell it where to DMA the send head, and it automatically
// does it, which means we read the send head from main memory instead of PCIe.
//
//
// Multiple send queues
// --------------------
// Supporting a single send queue is fine for some network functions that are inherently "pass-through", such as a VPN,
// but many functions need more than this, such as a MAC bridge.
// Extending the pipe model to work with multiple receive queues is not possible for the following reasons:
// - Multiple queues cannot use the same destination packets, since they could overwrite each other;
// - Since pipes process packets in order, assigning specific packets for reception by specific queues could cause
//   the pipe to halt waiting for a packet to be received even as other queues have received packets further in the pipe.
// Thus, we are left with the option of supporting multiple send queues.
// This is doable because transmit descriptors can be set to drop a packet by "sending" it with length zero;
// thus, the packet can be sent to whichever queues by setting its real length in these queues' descriptors,
// and not send to the other queues by setting a zero length on those queues' descriptors.
// In practice, this means we cannot use the same descriptor ring for all queues, though the contents only differ
// in the packet length of the send descriptors.
// But we can still share the ring between the receive queue and the first send queue.
// The overall send head is the earliest of the send queues' heads, and all send tails have the same value.
// Computing the former is non-trivial, because send heads are modulo the ring size;
// thus, the minimum of two send heads can only be computed with knowledge of another ring item;
// since we know the processed delimiter comes after the send heads,
// send_head_1 <% send_head_2 <=> (send_head_1 - processed_delimiter) < (send_head_2 - processed_delimiter)
// where <% represents "comes before in the modulo ring".
// =================================================================================================================

struct tn_net_pipe
{
	// Technically it's volatile since the hardware does DMA, but we don't access the buffer contents in the driver,
	// and when the packet is accessed by a handler the hardware cannot touch it due to how pipes work
	uint8_t* buffer;
	uintptr_t receive_tail_addr;
	uint64_t scheduling_counter;
	uint32_t processed_delimiter; // 32-bit since it's replicated to a NIC register
	uint8_t _padding[4 + 4*8];
	// send heads must be 16-byte aligned; see alignment remarks in send queue setup
	// (there is also a runtime check to make sure the array itself is aligned properly)
	// plus, we want each head on its own cache line to avoid conflicts
	// thus, using assumption CACHE, we multiply indices by 16
	#define SEND_HEAD_MULTIPLIER 16
	volatile uint32_t send_heads[IXGBE_PIPE_MAX_SENDS * SEND_HEAD_MULTIPLIER];
	volatile uint64_t* rings[IXGBE_PIPE_MAX_SENDS]; // 0 == shared receive/send, rest are exclusive send // TODO if we use 1gb hugepages we can alloc many rings, make this "base", and use base+n instead of storing separately
	uintptr_t send_tail_addrs[IXGBE_PIPE_MAX_SENDS];
};

// ===================
// Pipe initialization - no HW interactions
// ===================

bool tn_net_pipe_init(struct tn_net_pipe** out_pipe)
{
	uintptr_t buffer_addr;
	if (!tn_mem_allocate(IXGBE_RING_SIZE * IXGBE_PACKET_SIZE_MAX, &buffer_addr)) {
		TN_DEBUG("Could not allocate buffer for pipe");
		return false;
	}

	uintptr_t pipe_addr;
	if (!tn_mem_allocate(sizeof(struct tn_net_pipe), &pipe_addr)) {
		TN_DEBUG("Could not allocate pipe");
		return false;
	}

	struct tn_net_pipe* pipe = (struct tn_net_pipe*) pipe_addr;
	pipe->buffer = (uint8_t*) buffer_addr;

	for (uint64_t n = 0; n < IXGBE_PIPE_MAX_SENDS; n++) {
		// Rings need to be 128-byte aligned, as seen later
		static_assert(IXGBE_RING_SIZE * 16 >= 128, "The ring address should be 128-byte aligned");

		uintptr_t ring_addr; // TODO memory leaks... need to free this if subsequent ops fail
		if (!tn_mem_allocate(IXGBE_RING_SIZE * 16, &ring_addr)) { // 16 bytes per descriptor, i.e. 2x64bits
			TN_DEBUG("Could not allocate ring");
			return false;
		}

		pipe->rings[n] = (volatile uint64_t*) ring_addr;
	}

	*out_pipe = pipe;
	return true;
}


// ====================================
// Section 4.6.7 Receive Initialization
// ====================================

bool tn_net_pipe_set_receive(struct tn_net_pipe* const pipe, const struct tn_net_device* const device, const uint64_t long_queue_index)
{
	if (pipe->receive_tail_addr != 0) {
		TN_DEBUG("Pipe receive was already set");
		return false;
	}

	if (long_queue_index >= IXGBE_RECEIVE_QUEUES_COUNT) {
		TN_DEBUG("Receive queue does not exist");
		return false;
	}
	static_assert((uint8_t) -1 >= IXGBE_RECEIVE_QUEUES_COUNT, "This code assumes receive queues fit in an uint8_t");
	uint8_t queue_index = (uint8_t) long_queue_index;

	// See later for details of RXDCTL.ENABLE
	if (!IXGBE_REG_CLEARED(device->addr, RXDCTL, queue_index, ENABLE)) {
		TN_DEBUG("Receive queue is already in use");
		return false;
	}

	// "The following should be done per each receive queue:"
	// "- Allocate a region of memory for the receive descriptor list."
	// This is already done in pipe initialization as pipe->rings[0]
	// "- Receive buffers of appropriate size should be allocated and pointers to these buffers should be stored in the descriptor ring."
	// This will be done when setting up the first send ring
	// Note that only the first line (buffer address) needs to be configured, the second line is only for write-back except End Of Packet (bit 33)
	// and Descriptor Done (bit 32), which must be 0 as per table in "EOP (End of Packet) and DD (Descriptor Done)"
	// "- Program the descriptor base address with the address of the region (registers RDBAL, RDBAL)."
	// INTERPRETATION: This is a typo, the second "RDBAL" should read "RDBAH".
	// 	Section 8.2.3.8.1 Receive Descriptor Base Address Low (RDBAL[n]):
	// 	"The receive descriptor base address must point to a 128 byte-aligned block of data."
	// This alignment is guaranteed by the pipe initialization
	uintptr_t ring_phys_addr;
	if (!tn_mem_virt_to_phys((uintptr_t) pipe->rings[0], &ring_phys_addr)) {
		TN_DEBUG("Could not get phys addr of main ring");
		return false;
	}
	IXGBE_REG_WRITE(device->addr, RDBAH, queue_index, (uint32_t) (ring_phys_addr >> 32));
	IXGBE_REG_WRITE(device->addr, RDBAL, queue_index, (uint32_t) (ring_phys_addr & 0xFFFFFFFFu));
	// "- Set the length register to the size of the descriptor ring (register RDLEN)."
	// Section 8.2.3.8.3 Receive DEscriptor Length (RDLEN[n]):
	// "This register sets the number of bytes allocated for descriptors in the circular descriptor buffer."
	// Note that receive descriptors are 16 bytes.
	IXGBE_REG_WRITE(device->addr, RDLEN, queue_index, IXGBE_RING_SIZE * 16u);
	// "- Program SRRCTL associated with this queue according to the size of the buffers and the required header control."
	//	Section 8.2.3.8.7 Split Receive Control Registers (SRRCTL[n]):
	//		"BSIZEPACKET, Receive Buffer Size for Packet Buffer. The value is in 1 KB resolution. Value can be from 1 KB to 16 KB."
	// Set it to the ceiling of PACKET_SIZE_MAX in KB.
	// TODO: Play with this, see if it changes perf in any way.
	IXGBE_REG_WRITE(device->addr, SRRCTL, queue_index, BSIZEPACKET, IXGBE_PACKET_SIZE_MAX / 1024u + (IXGBE_PACKET_SIZE_MAX % 1024u != 0));
	//		"DESCTYPE, Define the descriptor type in Rx: Init Val 000b [...] 000b = Legacy."
	//		"Drop_En, Drop Enabled. If set to 1b, packets received to the queue when no descriptors are available to store them are dropped."
	// Enable this because of assumption DROP
	IXGBE_REG_SET(device->addr, SRRCTL, queue_index, DROP_EN);
	// "- If header split is required for this queue, program the appropriate PSRTYPE for the appropriate headers."
	// Section 7.1.10 Header Splitting: "Header Splitting mode might cause unpredictable behavior and should not be used with the 82599."
	// "- Program RSC mode for the queue via the RSCCTL register."
	// Nothing to do, we do not want RSC.
	// "- Program RXDCTL with appropriate values including the queue Enable bit. Note that packets directed to a disabled queue are dropped."
	IXGBE_REG_SET(device->addr, RXDCTL, queue_index, ENABLE);
	// "- Poll the RXDCTL register until the Enable bit is set. The tail should not be bumped before this bit was read as 1b."
	// INTERPRETATION: No timeout is mentioned here, let's say 1s to be safe.
	// TODO: Categorize all interpretations (e.g. "no timeout", "clear typo", ...)
	IF_AFTER_TIMEOUT(1000 * 1000, IXGBE_REG_CLEARED(device->addr, RXDCTL, queue_index, ENABLE)) {
		TN_DEBUG("RXDCTL.ENABLE did not set, cannot enable queue");
		return false;
	}
	// "- Bump the tail pointer (RDT) to enable descriptors fetching by setting it to the ring length minus one."
	// 	Section 7.1.9 Receive Descriptor Queue Structure:
	// 	"Software inserts receive descriptors by advancing the tail pointer(s) to refer to the address of the entry just beyond the last valid descriptor."
	IXGBE_REG_WRITE(device->addr, RDT, queue_index, IXGBE_RING_SIZE - 1u);
	// "- Enable the receive path by setting RXCTRL.RXEN. This should be done only after all other settings are done following the steps below."
	//	"- Halt the receive data path by setting SECRXCTRL.RX_DIS bit."
	IXGBE_REG_SET(device->addr, SECRXCTRL, _, RX_DIS);
	//	"- Wait for the data paths to be emptied by HW. Poll the SECRXSTAT.SECRX_RDY bit until it is asserted by HW."
	// INTERPRETATION: Another undefined timeout, assuming 1s as usual
	IF_AFTER_TIMEOUT(1000 * 1000, IXGBE_REG_CLEARED(device->addr, SECRXSTAT, _, SECRX_RDY)) {
		TN_DEBUG("SECRXSTAT.SECRXRDY timed out, cannot enable queue");
		return false;
	}
	//	"- Set RXCTRL.RXEN"
	IXGBE_REG_SET(device->addr, RXCTRL, _, RXEN);
	//	"- Clear the SECRXCTRL.SECRX_DIS bits to enable receive data path"
	// INTERPRETATION: This refers to RX_DIS, not SECRX_DIS, since it's RX_DIS being cleared that enables the receive data path.
	IXGBE_REG_CLEAR(device->addr, SECRXCTRL, _, RX_DIS);
	//	"- If software uses the receive descriptor minimum threshold Interrupt, that value should be set."
	// We do not have to set this by assumption NOWANT
	// "  Set bit 16 of the CTRL_EXT register and clear bit 12 of the DCA_RXCTRL[n] register[n]."
	// Section 8.2.3.1.3 Extended Device Control Register (CTRL_EXT): Bit 16 == "NS_DIS, No Snoop Disable"
	IXGBE_REG_SET(device->addr, CTRLEXT, _, NSDIS);
	// Section 8.2.3.11.1 Rx DCA Control Register (DCA_RXCTRL[n]): Bit 12 == "Default 1b; Reserved. Must be set to 0."
	IXGBE_REG_SET(device->addr, DCARXCTRL, queue_index, UNKNOWN);

	pipe->receive_tail_addr = device->addr + IXGBE_REG_RDT(queue_index);
	return true;
}


// =====================================
// Section 4.6.8 Transmit Initialization
// =====================================

bool tn_net_pipe_add_send(struct tn_net_pipe* const pipe, const struct tn_net_device* const device, const uint64_t long_queue_index)
{
	uint64_t send_queues_count = 0;
	for (; send_queues_count < IXGBE_PIPE_MAX_SENDS; send_queues_count++) {
		if (pipe->send_tail_addrs[send_queues_count] == 0) {
			break;
		}
	}
	if (send_queues_count == IXGBE_PIPE_MAX_SENDS) {
		TN_DEBUG("The pipe is already using the maximum amount of send queues");
		return false;
	}

	if (long_queue_index >= IXGBE_SEND_QUEUES_COUNT) {
		TN_DEBUG("Send queue does not exist");
		return false;
	}
	static_assert((uint8_t) -1 >= IXGBE_SEND_QUEUES_COUNT, "This code assumes send queues fit in an uint8_t");
	uint8_t queue_index = (uint8_t) long_queue_index;

	// See later for details of TXDCTL.ENABLE
	if (!IXGBE_REG_CLEARED(device->addr, TXDCTL, queue_index, ENABLE)) {
		TN_DEBUG("Send queue is already in use");
		return false;
	}

	// "The following steps should be done once per transmit queue:"
	// "- Allocate a region of memory for the transmit descriptor list."
	// This is already done in pipe initialization as pipe->rings[*]
	volatile uint64_t* ring = pipe->rings[send_queues_count];
	// Program all descriptors' buffer address now
	for (uintptr_t n = 0; n < IXGBE_RING_SIZE; n++) {
		// Section 7.2.3.2.2 Legacy Transmit Descriptor Format:
		// "Buffer Address (64)", 1st line offset 0
		uint8_t* packet = pipe->buffer + n * IXGBE_PACKET_SIZE_MAX;
		uintptr_t packet_phys_addr;
		if (!tn_mem_virt_to_phys((uintptr_t) packet, &packet_phys_addr)) {
			TN_DEBUG("Could not get a packet's phys addr");
		}
		ring[n * 2u] = packet_phys_addr;
	}
	// "- Program the descriptor base address with the address of the region (TDBAL, TDBAH)."
	// 	Section 8.2.3.9.5 Transmit Descriptor Base Address Low (TDBAL[n]):
	// 	"The Transmit Descriptor Base Address must point to a 128 byte-aligned block of data."
	// This alignment is guaranteed by the pipe initialization
	uintptr_t ring_phys_addr;
	if (!tn_mem_virt_to_phys((uintptr_t) ring, &ring_phys_addr)) {
		TN_DEBUG("Could not get a send ring's phys addr");
		return false;
	}
	IXGBE_REG_WRITE(device->addr, TDBAH, queue_index, (uint32_t) (ring_phys_addr >> 32));
	IXGBE_REG_WRITE(device->addr, TDBAL, queue_index, (uint32_t) (ring_phys_addr & 0xFFFFFFFFu));
	// "- Set the length register to the size of the descriptor ring (TDLEN)."
	// 	Section 8.2.3.9.7 Transmit Descriptor Length (TDLEN[n]):
	// 	"This register sets the number of bytes allocated for descriptors in the circular descriptor buffer."
	// Note that each descriptor is 16 bytes.
	IXGBE_REG_WRITE(device->addr, TDLEN, queue_index, IXGBE_RING_SIZE * 16u);
	// "- Program the TXDCTL register with the desired TX descriptor write back policy (see Section 8.2.3.9.10 for recommended values)."
	//	Section 8.2.3.9.10 Transmit Descriptor Control (TXDCTL[n]):
	//	"HTHRESH should be given a non-zero value each time PTHRESH is used."
	//	"For PTHRESH and HTHRESH recommended setting please refer to Section 7.2.3.4."
	// INTERPRETATION: The "recommended values" are in 7.2.3.4.1 very vague; we use (cache line size)/(descriptor size) for HTHRESH (i.e. 64/16 by assumption CACHE),
	//                 and a completely arbitrary value for PTHRESH (TODO bench with many, many, many different values)
	IXGBE_REG_WRITE(device->addr, TXDCTL, queue_index, PTHRESH, 60);
	IXGBE_REG_WRITE(device->addr, TXDCTL, queue_index, HTHRESH, 4);
	// "- If needed, set TDWBAL/TWDBAH to enable head write back."
	uintptr_t head_phys_addr;
	if (!tn_mem_virt_to_phys((uintptr_t) &(pipe->send_heads[send_queues_count * SEND_HEAD_MULTIPLIER]), &head_phys_addr)) {
		TN_DEBUG("Could not get the physical address of the send head");
		return false;
	}
	//	Section 7.2.3.5.2 Tx Head Pointer Write Back:
	//	"The low register's LSB hold the control bits.
	// 	 * The Head_WB_EN bit enables activation of tail write back. In this case, no descriptor write back is executed.
	// 	 * The 30 upper bits of this register hold the lowest 32 bits of the head write-back address, assuming that the two last bits are zero."
	//	"software should [...] make sure the TDBAL value is Dword-aligned."
	//	Section 8.2.3.9.11 Tx Descriptor completion Write Back Address Low (TDWBAL[n]): "the actual address is Qword aligned"
	// INTERPRETATION: Obvious contradiction here, Dword or Qword? Empirically, the answer is... 16 bytes. Write-back has no effect otherwise.
	if (head_phys_addr % 16u != 0) {
		TN_DEBUG("Send head's physical address is not aligned properly");
		return false;
	}
	//	Section 8.2.3.9.11 Tx Descriptor Completion Write Back Address Low (TDWBAL[n]):
	//	"Head_WB_En, bit 0 [...] 1b = Head write-back is enabled."
	//	"Reserved, bit 1"
	IXGBE_REG_WRITE(device->addr, TDWBAH, queue_index, (uint32_t) (head_phys_addr >> 32));
	IXGBE_REG_WRITE(device->addr, TDWBAL, queue_index, (uint32_t) ((head_phys_addr & 0xFFFFFFFFu) | 1));
	// Disable relaxed ordering of head pointer write-back, since it could cause the head pointer to be updated backwards
	// TODO: Can we not disable this? Or find a good reason in the data sheet why we should do this?
	IXGBE_REG_CLEAR(device->addr, DCATXCTRL, queue_index, TX_DESC_WB_RO_EN);
	// "- Enable transmit path by setting DMATXCTL.TE.
	//    This step should be executed only for the first enabled transmit queue and does not need to be repeated for any following queues."
	// Do it every time, it makes the code simpler.
	IXGBE_REG_SET(device->addr, DMATXCTL, _, TE);
	// "- Enable the queue using TXDCTL.ENABLE.
	//    Poll the TXDCTL register until the Enable bit is set."
	// INTERPRETATION: No timeout is mentioned here, let's say 1s to be safe.
	IXGBE_REG_SET(device->addr, TXDCTL, queue_index, ENABLE);
	IF_AFTER_TIMEOUT(1000 * 1000, IXGBE_REG_CLEARED(device->addr, TXDCTL, queue_index, ENABLE)) {
		TN_DEBUG("TXDCTL.ENABLE did not set, cannot enable queue");
		return false;
	}
	// "Note: The tail register of the queue (TDT) should not be bumped until the queue is enabled."
	// Nothing to transmit yet, so leave TDT alone.

	pipe->send_tail_addrs[send_queues_count] = device->addr + IXGBE_REG_TDT(queue_index);
	return true;
}


void tn_net_pipe_run_step(struct tn_net_pipe* pipe, tn_net_packet_handler* handler)
{
	pipe->scheduling_counter = pipe->scheduling_counter + 1u;

	if((pipe->scheduling_counter & (IXGBE_PIPE_SCHEDULING_PERIOD - 1)) == (IXGBE_PIPE_SCHEDULING_PERIOD - 1)) {
		// Race conditions are possible here, but we don't care since all they can do is change the "real" value
		// of the earliest send head to a later value, which is fine.
		uint32_t earliest_send_head = 0;
		uint64_t min_diff = (uint64_t) -1;
		for (uint64_t n = 0; n < IXGBE_PIPE_MAX_SENDS; n++) {
			bool head_is_used = pipe->send_tail_addrs[n] != 0;
			if (head_is_used) {
				uint32_t head = pipe->send_heads[n * SEND_HEAD_MULTIPLIER];
				uint64_t diff = head - pipe->processed_delimiter; // TODO it'd be nice if we didn't need it here so we could modulo it only when writing, not at every iter
				if (diff < min_diff) {
					earliest_send_head = head;
					min_diff = diff;
				}

				ixgbe_reg_write_raw(pipe->send_tail_addrs[n], pipe->processed_delimiter);
			}
		}

		ixgbe_reg_write_raw(pipe->receive_tail_addr, (earliest_send_head - 1) & (IXGBE_RING_SIZE - 1));
	}

	// Since descriptors are 16 bytes, the index must be doubled
	volatile uint64_t* main_metadata_addr = pipe->rings[0] + 2u*pipe->processed_delimiter + 1;
	uint64_t receive_metadata = *main_metadata_addr;
	// Section 7.1.5 Legacy Receive Descriptor Format:
	// "Status Field (8-bit offset 32, 2nd line)": Bit 0 = DD, "Descriptor Done."
	if ((receive_metadata & BITL(32)) == 0) {
		return;
	}

	// This cannot overflow because the packet is by definition in an allocated block of memory
	uint8_t* packet = pipe->buffer + (IXGBE_PACKET_SIZE_MAX * pipe->processed_delimiter);
	// "Length Field (16-bit offset 0, 2nd line): The length indicated in this field covers the data written to a receive buffer."
	uint16_t receive_packet_length = receive_metadata & 0xFFu;
	bool send_list[IXGBE_PIPE_MAX_SENDS] = {0};
	uint16_t send_packet_length = handler(packet, receive_packet_length, send_list);

	// Section 7.2.3.2.2 Legacy Transmit Descriptor Format:
	// "Buffer Address (64)", 1st line
	// 2nd line:
		// "Length", bits 0-15: "Length (TDESC.LENGTH) specifies the length in bytes to be fetched from the buffer address provided"
			// "Note: Descriptors with zero length (null descriptors) transfer no data."
		// "CSO", bits 16-23: "A Checksum Offset (TDESC.CSO) field indicates where, relative to the start of the packet, to insert a TCP checksum if this mode is enabled"
			// All zero
		// "CMD", bits 24-31:
			// "RSV (bit 7) - Reserved
			//  VLE (bit 6) - VLAN Packet Enable [...]
			//  DEXT (bit 5) - Descriptor extension (zero for legacy mode)
			//  RSV (bit 4) - Reserved
			//  RS (bit 3) - Report Status: "signals hardware to report the DMA completion status indication"
			//  IC (bit 2) - Insert Checksum - Hardware inserts a checksum at the offset indicated by the CSO field if the Insert Checksum bit (IC) is set.
			//  IFCS (bit 1) — Insert FCS:
			//	"There are several cases in which software must set IFCS as follows: -Transmitting a short packet while padding is enabled by the HLREG0.TXPADEN bit."
			//      Section 8.2.3.22.8 MAC Core Control 0 Register (HLREG0): "TXPADEN, init val 1b; 1b = Pad frames"
			//  EOP (bit 0) - End of Packet"
		// STA, bits 32-35: "DD (bit 0) - Descriptor Done. The other bits in the STA field are reserved."
			// All zero
		// Rsvd, bits 36-39: "Reserved."
			// All zero
		// CSS, bits 40-47: "A Checksum Start (TDESC.CSS) field indicates where to begin computing the checksum."
			// All zero
		// VLAN, bits 48-63:
			// All zero
	// The buffer address does not get clobbered by write-back, so no need to set it again
	// This means all we have to do is set the length in the first 16 bits, then bits 0,1,3 of CMD
	// Importantly, since bit 32 will stay at 0, and we share the receive ring and the first send ring, it will clear the Descriptor Done flag of the receive descriptor
	// If not all send rings are used, we will write into an unused (but allocated!) ring, that's fine
	for (uint64_t n = 0; n < IXGBE_PIPE_MAX_SENDS; n++) {
		*(pipe->rings[n] + 2u*pipe->processed_delimiter + 1) = (send_list[n] * (uint64_t) send_packet_length) | BITL(24+3) | BITL(24+1) | BITL(24);
	}

	// Increment the processed delimiter, modulo the ring size
	pipe->processed_delimiter = (pipe->processed_delimiter + 1u) & (IXGBE_RING_SIZE - 1);
}
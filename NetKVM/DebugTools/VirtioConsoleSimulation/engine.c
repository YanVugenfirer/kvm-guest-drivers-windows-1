#include "osdep.h"
#include "VirtIO_Win.h"
#include "testcommands.h"
#include "IONetDescriptor.h"
#include "Hardware\Hardware.h"

#define VIRTIO_NET_INVALID_INTERRUPT_STATUS		0xFF

#define MAX_RX_PACKET			1024
#define MAX_TX_PACKET			1024

BOOLEAN bUseMergedBuffers = TRUE;
BOOLEAN bUsePublishedIndices = TRUE;
BOOLEAN bHostHasVnetHdr = TRUE;
BOOLEAN bVirtioF_NotifyOnEmpty = FALSE;
BOOLEAN bAsyncTransmit = FALSE;
BOOLEAN bUseIndirectTx = FALSE;
BOOLEAN bMSIXUsed = FALSE;

int debugLevel = 4;

typedef struct
{
	virtio_net_hdr_basic header;
	UCHAR  buffer[MAX_RX_PACKET];
} tRxPacketBasic;

typedef struct
{
	virtio_net_hdr_ext extheader;
	UCHAR  buffer[MAX_RX_PACKET];
} tRxPacketExt;

typedef struct
{
	union
	{
		tRxPacketBasic basic;
		tRxPacketExt ext;
	};
	ULONG serial;
} tRxPacket;

typedef struct
{
	virtio_net_hdr_ext header;
	ULONG serial;
	UCHAR  buffer[MAX_TX_PACKET];
	void *storageForIndirect;
} tTxPacket;

typedef struct
{
	virtio_net_hdr_ext header;
	UCHAR  buffer[128];
} tAuxPacket;


typedef struct
{
	VirtIODevice *dev;
	ULONG hostFeatures;
} tHost;

typedef struct
{
	PVOID originalAddress;
	PVOID hardwareDevice;
//	USHORT queueSizes[4];
	UCHAR queueSelect;
} tDevice, *ptDevice;


static void DeviceReset(tDevice *device)
{
	device->queueSelect = 0;
}

u32 ReadVirtIODeviceRegister(ULONG_PTR ulRegister)
{
	u32 val = 0xffffffff;
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	switch (reg)
	{
		case VIRTIO_PCI_QUEUE_PFN:
			val = hwGetQueuePfn(device->hardwareDevice, device->queueSelect);
			break;
		default:
			FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			break;
	}
	DPrintf(0, ("DW[%d] => %x\n", reg, val));
	return val;
}

void WriteVirtIODeviceRegister(ULONG_PTR ulRegister, u32 ulValue)
{
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	DPrintf(0, ("DW %x => %d\n", ulValue, reg));
	switch (reg)
	{
		case VIRTIO_PCI_QUEUE_PFN:
			hwSetQueuePfn(device->hardwareDevice, device->queueSelect, ulValue);
			break;
		default:
			FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			break;
	}
}

u8 ReadVirtIODeviceByte(ULONG_PTR ulRegister)
{
	UCHAR val = 0xff;
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	switch (reg)
	{
		case VIRTIO_PCI_ISR:
			val = hwReadInterruptStatus(device->hardwareDevice);
			break;
		case VIRTIO_PCI_STATUS:
			val = hwGetDeviceStatus(device->hardwareDevice);
			break;
		default:
			if (!hwReadDeviceData(device->hardwareDevice, reg, &val))
			{
				FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			}
			break;
	}
	DPrintf(0, ("B[%d] => %x\n", reg, val));
	return val;
}

void WriteVirtIODeviceByte(ULONG_PTR ulRegister, u8 bValue)
{
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	DPrintf(0, ("B %x => %d\n", bValue, reg));
	switch (reg)
	{
		case VIRTIO_PCI_STATUS:
			hwSetDeviceStatus(device->hardwareDevice, bValue);
			if (bValue) DeviceReset(device);
			break;
		default:
			if (!hwWriteDeviceData(device->hardwareDevice, reg, bValue))
			{
				FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			}
			break;
	}
}

u16 ReadVirtIODeviceWord(ULONG_PTR ulRegister)
{
	u16 val = 0xffff;
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	switch (reg)
	{
		case VIRTIO_PCI_QUEUE_SEL:
			val = device->queueSelect;
			break;
		case VIRTIO_PCI_QUEUE_NUM:
			val = hwGetQueueSize(device->hardwareDevice, device->queueSelect);
			break;
		default:
			FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			break;
	}
	DPrintf(0, ("W[%d] => %x\n", reg, val));
	return val;
}

void WriteVirtIODeviceWord(ULONG_PTR ulRegister, u16 wValue)
{
	ULONG reg = ulRegister & 0x1f;
	tDevice *device = (tDevice *)(ulRegister - reg);
	DPrintf(0, ("W %x => %d\n", wValue, reg));
	switch (reg)
	{
		case VIRTIO_PCI_QUEUE_SEL:
			device->queueSelect = (UCHAR)wValue;
			break;
		case VIRTIO_PCI_QUEUE_NOTIFY:
			hwQueueNotify(device->hardwareDevice, wValue);
			break;
		default:
			FailCase("%s(%d) - not supported", __FUNCTION__, reg);
			break;
	}
}

#if 0

PVOID AllocatePhysical(ULONG size)
{
	ULONG_PTR addr, base;
	PVOID pRet, p = malloc(size + 2 * PAGE_SIZE);
	DPrintf(0, ("asked for %d, allocated %p\n", size, p));
	addr = (ULONG_PTR)p;
	base = (addr + PAGE_SIZE) & ~(PAGE_SIZE - 1);
	*(PVOID *)base = p;
	pRet = (PVOID)(base + PAGE_SIZE);
	DPrintf(0, ("returning %p\n", pRet));
	return pRet;
}

static void MmFreeContiguousMemory(PVOID virtualAddress)
{
	PVOID actualAddr;
	ULONG_PTR addr = (ULONG_PTR)virtualAddress;
	DPrintf(0, ("asked to free %p\n", addr));
	addr -= PAGE_SIZE;
	actualAddr = *(PVOID *)addr;
	DPrintf(0, ("freeing %p\n", actualAddr));
	free(actualAddr);
}

#else

PVOID AllocatePhysical(ULONG size)
{
	ULONG_PTR addr, base;
	PVOID pRet, p = malloc(size + 2 * PAGE_SIZE);
	DPrintf(0, ("asked for %d, allocated %p\n", size, p));
	addr = (ULONG_PTR)p;
	base = (addr + PAGE_SIZE) & ~(PAGE_SIZE - 1);
	*(PVOID *)base = p;
	pRet = (PVOID)(base + PAGE_SIZE);
	DPrintf(0, ("returning %p\n", pRet));
	return pRet;
}

static void MmFreeContiguousMemory(PVOID virtualAddress)
{
	PVOID actualAddr;
	ULONG_PTR addr = (ULONG_PTR)virtualAddress;
	DPrintf(0, ("asked to free %p\n", addr));
	addr -= PAGE_SIZE;
	actualAddr = *(PVOID *)addr;
	DPrintf(0, ("freeing %p\n", actualAddr));
	free(actualAddr);
}

#endif

static PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID virtualAddress)
{
	PHYSICAL_ADDRESS pa;
	pa.QuadPart = (UINT_PTR)virtualAddress;
	return pa;
}



static void InitializeDevice(tDevice *pDev)
{
	pDev->hardwareDevice = hwCreateDevice(pDev);
}



static struct virtqueue *TxQ;
static struct virtqueue *RxQ;
static struct virtqueue *AuxQ;

tDevice *pDevice;
static tHost Host;

void GetRxBuffer(PULONG pLenght)
{
	UINT len;
	ULONG serial = 0;
	tRxPacket *p = (tRxPacket *)RxQ->vq_ops->get_buf(RxQ, &len);
	if (p)
	{
		serial = p->serial;
		if (bUseMergedBuffers) *pLenght = len - sizeof(p->ext.extheader);
		else *pLenght = len - sizeof(p->basic.header);
		KeepRxPacket(p, serial);
	}
	else
	{
		FailCase("%s", __FUNCTION__);
	}
}

static BOOLEAN AddRxBuffer(tRxPacket *pPacket, BOOLEAN bKick)
{
	BOOLEAN bOK = TRUE;
	if (bUseMergedBuffers)
	{
		struct VirtIOBufferDescriptor sg;
		sg.physAddr = MmGetPhysicalAddress(pPacket);
		sg.ulSize = sizeof(pPacket->ext);
		bOK = 0 <= RxQ->vq_ops->add_buf(RxQ, &sg, 0, 1, pPacket, NULL, 0);
	}
	else
	{
		struct VirtIOBufferDescriptor sg[2];
		sg[0].physAddr = MmGetPhysicalAddress(&pPacket->basic.header);
		sg[0].ulSize = sizeof(pPacket->basic.header);
		sg[1].physAddr = MmGetPhysicalAddress(pPacket->basic.buffer);
		sg[1].ulSize = sizeof(pPacket->basic.buffer);
		bOK = 0 <= RxQ->vq_ops->add_buf(RxQ, sg, 0, 2, pPacket, NULL, 0);
	}

	if (!bOK)
	{
		LogTestFlow("[%s] Can't add buffer %d\n", __FUNCTION__, pPacket->serial);
	}
	else if (bKick)
	{
		RxQ->vq_ops->kick(RxQ);
	}
	return bOK;
}

static BOOLEAN AddAuxBuffer(ULONG size)
{
	UINT i;
	BOOLEAN bOK;
	PUCHAR p = (PUCHAR)malloc(size);
	struct VirtIOBufferDescriptor sg;
	for (i = 0; i < size; ++i) p[i] = 'A' + i;
	sg.physAddr = MmGetPhysicalAddress(p);
	sg.ulSize = size;
	bOK = 0 <= AuxQ->vq_ops->add_buf(AuxQ, &sg, 1, 0, p, NULL, 0);
	if (!bOK)
	{
		FailCase("[%s] failed");
	}
	else
	{
		AuxQ->vq_ops->kick(AuxQ);
	}
}

void ReturnRxBuffer(ULONG serial)
{
	tRxPacket *pPacket = (tRxPacket *)GetRxPacket(serial);

	if (pPacket) AddRxBuffer(pPacket, TRUE);
}

static BOOLEAN AddTxBuffer(ULONG serial)
{
	BOOLEAN bOK = TRUE;
	ULONGLONG hwIndirectAddress = 0;
	ULONG headerSize = bUseMergedBuffers ? sizeof(virtio_net_hdr_ext) : sizeof(virtio_net_hdr_basic);
	struct VirtIOBufferDescriptor sg[2];
	tTxPacket *pPacket = (tTxPacket *)malloc(sizeof(tTxPacket));
	memset(pPacket, 0, sizeof(tTxPacket));
	pPacket->serial = serial;
	sg[0].physAddr = MmGetPhysicalAddress(&pPacket->header);
	sg[0].ulSize = headerSize;
	sg[1].physAddr = MmGetPhysicalAddress(pPacket->buffer);
	sg[1].ulSize = sizeof(pPacket->buffer);
	if (bUseIndirectTx)
	{
		pPacket->storageForIndirect = AllocatePhysical(2 * 16);
		hwIndirectAddress = MmGetPhysicalAddress(pPacket->storageForIndirect).QuadPart;
	}
	if (0 > TxQ->vq_ops->add_buf(TxQ, sg, 2, 0, pPacket, pPacket->storageForIndirect, hwIndirectAddress))
	{
		free(pPacket);
		bOK = FALSE;
	}
	return bOK;
}

void AddTxBuffers(ULONG startSerial, ULONG num)
{
	BOOLEAN bOK = TRUE;
	while (num-- && bOK)
	{
		bOK = AddTxBuffer(startSerial);
		if (bOK) startSerial++;
	}

	if (!bOK) FailCase("[%s] can't add buffer %d", __FUNCTION__, startSerial);
	else
		TxQ->vq_ops->kick(TxQ);
	hwCheckInterrupt(pDevice->hardwareDevice);
}


void GetTxBuffer(ULONG serial)
{
	UINT len = 0;
	tTxPacket *p = (tTxPacket *)TxQ->vq_ops->get_buf(TxQ, &len);
	if (p)
	{
		if (serial != p->serial)
		{
			FailCase("[%s] got %d, expected %d", __FUNCTION__, p->serial, serial);
		}
		else if (!len)
		{
			//FailCase("[%s] got invalid packet", __FUNCTION__);
		}
		if (p->storageForIndirect) free(p->storageForIndirect);
		free(p);
	}
	else
	{
		FailCase("[%s] - no buffer", __FUNCTION__);
	}
}

void SimulationPrepare()
{
	ULONG i, size, allocSize, numQueueus;
	pDevice = (tDevice *)AllocatePhysical(sizeof(tDevice));
	memset(pDevice, 0, sizeof(*pDevice));

	numQueueus = AUX_QUEUE_NUMBER + 1;

	InitializeDevice(pDevice);

	allocSize = VirtIODeviceSizeRequired((USHORT)numQueueus);
	Host.dev = (VirtIODevice *)malloc(allocSize);

	VirtIODeviceInitialize(Host.dev, (ULONG_PTR)pDevice, allocSize);
	VirtIODeviceSetMSIXUsed(Host.dev, bMSIXUsed);
	VirtIODeviceQueryQueueAllocation(Host.dev, TX_QUEUE_NUMBER, &size, &allocSize);
	if (allocSize)
	{
		PVOID p = AllocatePhysical(allocSize);
		if (p)
		{
			PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(p);
			TxQ = VirtIODevicePrepareQueue(Host.dev, TX_QUEUE_NUMBER, phys, p, allocSize, p);
		}
	}

	VirtIODeviceQueryQueueAllocation(Host.dev, RX_QUEUE_NUMBER, &size, &allocSize);
	if (allocSize)
	{
		PVOID p = AllocatePhysical(allocSize);
		if (p)
		{
			PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(p);
			RxQ = VirtIODevicePrepareQueue(Host.dev, RX_QUEUE_NUMBER, phys, p, allocSize, (PUCHAR)p + 1);
		}
	}

	VirtIODeviceQueryQueueAllocation(Host.dev, AUX_QUEUE_NUMBER, &size, &allocSize);
	if (allocSize)
	{
		PVOID p = AllocatePhysical(allocSize);
		if (p)
		{
			PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(p);
			AuxQ = VirtIODevicePrepareQueue(Host.dev, AUX_QUEUE_NUMBER, phys, p, allocSize, (PUCHAR)p - 1);
		}
	}


	if (TxQ && RxQ && AuxQ)
	{
		size = VirtIODeviceGetQueueSize(RxQ);
		for (i = 0; i < size; ++i)
		{
			tRxPacket *pPacket = (tRxPacket *)malloc(sizeof(tRxPacket));
			memset(pPacket, 0, sizeof(tRxPacket));
			pPacket->serial = i;
			if (!AddRxBuffer(pPacket, FALSE))
			{
				//FailCase("[%s] - filling Rx", __FUNCTION__);
				break;
			}
		}
		DPrintf(0, ("added %d blocks\n", i));
		RxQ->vq_ops->kick(RxQ);

		AddAuxBuffer(8);
	}
	else
	{
		FailCase("[%s] - queues", __FUNCTION__);
	}
}

void	SimulationFinish()
{
	if (TxQ)
	{
		TxQ->vq_ops->shutdown(TxQ);
		VirtIODeviceDeleteQueue(TxQ, NULL);
	}
	if (RxQ)
	{
		PVOID va = NULL;
		RxQ->vq_ops->shutdown(RxQ);
		VirtIODeviceDeleteQueue(RxQ, &va);
		if (va) MmFreeContiguousMemory((PUCHAR)va - 1);
	}
	if (AuxQ)
	{
		PVOID va = NULL;
		AuxQ->vq_ops->shutdown(AuxQ);
		VirtIODeviceDeleteQueue(AuxQ, &va);
		if (va) MmFreeContiguousMemory((PUCHAR)va + 1);
	}

	if (pDevice)
	{
		ULONG tx = 0, rx = 0;
		hwGetInterrups(pDevice->hardwareDevice, &tx, &rx);
		if (tx || rx)
		{
			LogTestFlow("Interrupts: TX:%d, RX:%d\n", tx, rx);
		}
		hwDestroyDevice(pDevice->hardwareDevice);
		MmFreeContiguousMemory(pDevice);
	}
	free(Host.dev);
}


void KickTx(void)
{
	TxQ->vq_ops->kick(TxQ);
}

void KickTxAlways(void)
{
	TxQ->vq_ops->kick_always(TxQ);
}

BOOLEAN TxRestart(void)
{
	return TxQ->vq_ops->restart(TxQ);
}

BOOLEAN RxRestart(void)
{
	return RxQ->vq_ops->restart(RxQ);
}

void TxEnableInterrupt(BOOLEAN bEnable)
{
	TxQ->vq_ops->enable_interrupt(TxQ, bEnable);
}

void RxEnableInterrupt(BOOLEAN bEnable)
{
	RxQ->vq_ops->enable_interrupt(RxQ, bEnable);
}

EXTERN_C void RxReceivePacket(UCHAR fill)
{
	void *buffer;
	ULONG size;
	if (!bHostHasVnetHdr)
	{
		size = MAX_RX_PACKET;
		buffer = malloc(MAX_RX_PACKET);
		memset(buffer, fill, size);
	}
	else
	{
		tRxPacketBasic *pp;
		size = sizeof(tRxPacketBasic);
		pp = (tRxPacketBasic *)malloc(size);
		memset(&pp->header, 0, sizeof(pp->header));
		memset(&pp->buffer, fill, sizeof(pp->buffer));
		buffer = pp;
	}
	hwReceiveBuffer(pDevice->hardwareDevice, buffer, size);
	free(buffer);
}

EXTERN_C void CompleteTx(int num)
{
	hwCompleteTx(pDevice->hardwareDevice, num);
}

EXTERN_C UCHAR GetDeviceData(UCHAR offset)
{
	UCHAR val;
	VirtIODeviceGet(Host.dev, offset, &val, 1);
	return val;
}

EXTERN_C void SetDeviceData(UCHAR offset, UCHAR val)
{
	VirtIODeviceSet(Host.dev, offset, &val, 1);
}

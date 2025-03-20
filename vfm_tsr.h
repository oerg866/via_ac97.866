#ifndef _VFM_TSR_H_
#define _VFM_TSR_H_

#define VFM_TSR_SIGNATURE_1 0x08660866
#define VFM_TSR_SIGNATURE_2 0xAC97AC97

#include "pci.h"
#include "types.h"

#ifdef DEBUG
#include <stdio.h>
#define DBG_PRINT printf
#else
/* dunno how to do this more elegantly... */
#pragma warning(disable: 4002)
#define DBG_PRINT()
#endif

#pragma pack (1)
typedef struct {
    u32 size;
    u32 offset;
    u16 segment;
    u16 bufferId;
    u32 physAddr;
} vfm_VirtualDmaDescriptor;
#pragma pack()

void sys_outPortB(u16 port, u8 outVal);
u8 sys_inPortB(u16 port);

/* Hardware/IRQ/Mem init & dma engine start */
bool vfm_tsrInitialize(pci_Device dev);
/* Hardware/IRQ/Mem deinit & dma engine stop */
void vfm_tsrCleanup();

void vfm_tsrPrintStatus(bool cr);
/* Make program resident (becoming a TSR) - NOT FUNCTIONAL YET */
void vfm_terminateAndStayResident();
/* Unload the TSR - NOT FUNCTIONAL YET */
void vfm_tsrUnload();

/* Gets (and resets if set) the flag signaling whether the DMA interrupt has occurred */
bool vfm_tsrHasIntOccurred();
/* Gets DMA engine's #<index> buffer write pointer, negative index = current one */
i16 *vfm_tsrGetDmaBufferWritePtr(i16 index);
/* Gets size of a DMA buffer block */
u16 vfm_tsrGetDmaBufferSize();
/* Gets the amount of DMA buffers in use */
u16 vfm_tsrGetDmaBufferCount();
/* Gets size of TSR's NMI handler */
u16 vfm_tsrGetNmiHandlerSize();
/* Gets size of the whole TSR */
u16 vfm_tsrGetTsrSize();

/* Custom puts method to avoid MS C Library usage */
void vfm_puts(const char *str);

/* Checks if Virtual DMA Services (VDS) are supported */
bool vfm_vdsIsSupported();
/* Locks a DMA region using VDS */
bool vfm_vdsLockDmaRegion(vfm_VirtualDmaDescriptor *dds, u16 flags);
/* Unlocks a DMA region using VDS */
bool vfm_vdsUnlockDmaRegion(vfm_VirtualDmaDescriptor *dds);

#endif

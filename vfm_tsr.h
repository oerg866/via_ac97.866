#ifndef _VFM_TSR_H_
#define _VFM_TSR_H_

#define VFM_TSR_SIGNATURE_1 0x08660866
#define VFM_TSR_SIGNATURE_2 0xAC97AC97

#include "pci.h"
#include "types.h"

void sys_outPortB(u16 port, u8 outVal);
u8 sys_inPortB(u16 port);

/* Hardware/IRQ/Mem init & dma engine start */
void vfm_tsrInitialize(pci_Device dev);
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
#endif

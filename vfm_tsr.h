#ifndef _VFM_TSR_H_
#define _VFM_TSR_H_

#define VFM_TSR_SIGNATURE_1 0x08660866
#define VFM_TSR_SIGNATURE_2 0xAC97AC97

#include "pci.h"
#include "types.h"

/* Hardware/IRQ/Mem init & dma engine start */
void vfm_tsrInitialize(pci_Device dev);
/* Hardware/IRQ/Mem deinit & dma engine stop */
void vfm_tsrCleanup();

/* Make program resident (becoming a TSR) - NOT FUNCTIONAL YET */
void vfm_terminateAndStayResident(void);
/* Unload the TSR - NOT FUNCTIONAL YET */
void vfm_tsrUnload(void);

/* Gets (and resets if set) the flag signaling whether the DMA interrupt has occurred */
bool vfm_tsrHasIntOccurred();
/* Gets DMA engine's current buffer write pointer */
i16 *vfm_tsrGetDmaBufferWritePtr();
/* Gets size of a DMA buffer block */
u16 vfm_tsrGetDmaBufferSize();
/* Gets size of TSR's NMI handler */
u16 vfm_tsrGetNmiHandlerSize();
/* Gets size of the whole TSR */
u16 vfm_tsrGetTsrSize(void);
#endif

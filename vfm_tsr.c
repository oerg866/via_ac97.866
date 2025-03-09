/* VIA_AC97.866 FM Emulation TSR
 * 
 * (C) 2025 Eric Voirin (Oerg866)
 *
 * LICENSE: CC-BY-NC-SA 4.0
 *
 * FM Emulation TSR Data & C Code
 */

#include "vfm_tsr.h"
#include "v97_reg.h"
#include "386asm.h"
#include "types.h"

#include <stddef.h>
#include <dos.h>

#ifdef LOGGING
#include <io.h>
#include <conio.h>
#include <stdio.h>
#endif

#include "nukedopl/opl3.h"

#define UNUSED_ARG(_arg_) (void) _arg_

typedef void (_interrupt _far *IRQHANDLER)(void);

#define FM_PCM_SAMPLE_RATE 24000
#define STEREO 2
#define DMA_ALIGN 8
#define FM_PCM_BUFFER_ALLOC_SIZE (sizeof(i16) * DMA_SAMPLES_PER_BUFFER * STEREO)
#define VFM_MEM_POOL_SIZE (sizeof(v97_SgdTableEntry) * DMA_BUFFER_COUNT + FM_PCM_BUFFER_ALLOC_SIZE * DMA_BUFFER_COUNT + DMA_ALIGN)

/*  Static memory pools for DMA table and FM DMA buffers
    The reason we do it this way is because traditional "code models" in DOS don't support arbitrary data alignments
    so we allocate a memory pool larger than what's needed and then programatically set the pointers */

u8                  g_vfm_fmDmaMemPool[VFM_MEM_POOL_SIZE];  /* Memory poool for SGD Table + Buffers + Alignmnet reserve */
v97_SgdTableEntry  *g_vfm_fmDmaTable = NULL;                /* Pointer to SGD Table */
u32                 g_vfm_fmDmaTablePhysAddress = 0;        /* Physical 32-Bit address of SGD table */
i16                *g_vfm_fmDmaBuffers[DMA_BUFFER_COUNT];   /* Pointer list for all DMA buffers */

IRQHANDLER          g_vfm_oldPciIsr = NULL;                 /* Previous Interrupt Handler for device IRQ */
IRQHANDLER          g_vfm_oldNmiIsr = NULL;                 /* Previous Interrupt Handler for NMI */

pci_Device          g_vfm_pciDevice;                        /* PCI audio device structure (bus, slot, function) */
u16                 g_vfm_pciIrq    = 0;                    /* Interrupt of the PCI Audio Device */
bool                g_vfm_slaveIrq;                         /* Device is on master/slave PIC (true if IRQ > 7) */
u16                 g_vfm_ioBaseDma = 0;                    /* Base I/O port for Audio Codec / SGD Interface */
u16                 g_vfm_ioBaseNmi = 0;                    /* Base I/O port for FM NMI Status / Data */

opl3_chip           g_vfm_oplChip;                          /* OPL Emulator structure */

/* Definitions from vfm_isr.asm */
extern u16          g_DMA_IRQOccured;                       /* Flag by ISR when device IRQ has occured *and* was handled by us */
extern u16          g_DMA_BufferIndex;                      /* Buffer Index currently used by DMA engine for writing */

extern void _far _interrupt vfm_dmaInterruptHandler(void);  /* Our ISR for device IRQ */
extern void _far _interrupt vfm_nmiHandler(void);           /* Our ISR for NMI */
extern void         vfm_nmiHandlerEnd(void);                /* Dummy function for a pointer to end of the NMI */


/* START FM SGD DMA playback stream */
static void vfm_tsrStartDma() {
    v97_SgdChannelType sgdType;
    v97_SgdChannelCtrl sgdCtrl;

    sgdType.raw = 0;
    sgdType.intOnFlag = 1;              /* PCI Interrupt on FLAG */
    sgdType.intOnEOL = 1;               /* PCI Interrupt on EOL */
    sgdType.intSelect = 0;              /* Interrupt at PCI Read of Last Line */
    sgdType.autoStartSgdAtEOL = 1;      /* Auto-Restart DMA when last block is played (EOL) */
    outp(g_vfm_ioBaseDma + V97_FM_SGD_TYPE, sgdType.raw);
    
    sgdCtrl.raw = 0;
    sgdCtrl.start = 1;                  /* SGD Start */
    outp(g_vfm_ioBaseDma + V97_FM_SGD_CTRL, sgdCtrl.raw);

    sys_ioDelay(1000);

    printf("[FM Audio Stream Started]\n");
}

/* STOP FM SGD DMA playback stream */
static void vfm_tsrStopDma() {
    v97_SgdChannelCtrl sgdCtrl;

    sgdCtrl.raw = 0;
    sgdCtrl.terminate = 1;
    outp(g_vfm_ioBaseDma + V97_FM_SGD_CTRL, sgdCtrl.raw);

    sys_ioDelay(1000);
    
    printf("[FM Audio Stream Stopped]\n");
}

/* Sets up device interrupt vector & mask as well as NMI vector */
static void vfm_tsrSetupInterrupts() {
    u8  mask = ~(1 << (g_vfm_pciIrq & 0x07));
    u16 vector;
    u16 imrPort;

    _disable();

    if (g_vfm_pciIrq > 7) {
        vector  = g_vfm_pciIrq + 0x68;
        imrPort = 0xA1;
        g_vfm_slaveIrq = true;
    } else {
        vector  = g_vfm_pciIrq + 8;
        imrPort = 0x21;
    }
    
    /* Unmask IRQ in Interrupt Mask Register */
    outp(imrPort, mask & inp(imrPort));
    
    /* Get old vectors & set new ones */
    g_vfm_oldPciIsr = (IRQHANDLER) _dos_getvect(vector);
    g_vfm_oldNmiIsr = (IRQHANDLER) _dos_getvect(0x02);

    _dos_setvect(vector, vfm_dmaInterruptHandler);
    _dos_setvect(0x02,   vfm_nmiHandler);
    
    
    _enable();
}

/* Restores device interrupt vector & mask as well as NMI vector */
static void vfm_tsrRestoreInterrupts() {
    u8  mask = 1 << (g_vfm_pciIrq & 0x07);
    u16 vector;
    u16 imrPort;

    _disable();


    if (g_vfm_pciIrq > 7) {
        vector  = g_vfm_pciIrq + 0x68;
        imrPort = 0xA1;
    } else {
        vector  = g_vfm_pciIrq + 8;
        imrPort = 0x21;
    }

    /* Mask IRQ in Interrupt Mask Register */

    outp(imrPort, mask | inp(imrPort));

    /* Restore old ISRs */
    _dos_setvect(vector, g_vfm_oldPciIsr);
    _dos_setvect(0x02,   g_vfm_oldNmiIsr);

    _enable();
}

/*  Sets PCI registers so that we can... work :P */
static void vfm_tsrSetupPCIRegisters() {
    v97_FmNmiCtrl   fmNmiCtrl;  /* FM NMI Control Register */
    v97_ACLinkCtrl  acLinkCtrl; /* AC-Link Control Register */

    fmNmiCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL);
    fmNmiCtrl.fmSgdDataForSbMixing = true;  /* FM PCM stream responds to SB mixer settings */
    fmNmiCtrl.fmIrqSelect = 0;              /* NMI, not SMI - TODO: Make this configurable? SMI may be more compatible */
    fmNmiCtrl.fmTrapIntDisable = 0;         /* 0 = enable FM Trap interrupt */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL, fmNmiCtrl.raw);

    acLinkCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL);
    acLinkCtrl.fmPcmDataOutput = true;      /* Enable FM PCM stream playback via AC-Link */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL, acLinkCtrl.raw);
}

/*  Un-sets the PCI registers we set during init */
static void vfm_tsrUnsetPCIRegisters() {
    v97_FmNmiCtrl   fmNmiCtrl;  /* FM NMI Control Register */
    v97_ACLinkCtrl  acLinkCtrl; /* AC-Link Control Register */

    fmNmiCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL);
    
    fmNmiCtrl.fmSgdDataForSbMixing = false; /* FM PCM stream doesn't respond to SB mixer settings */
    fmNmiCtrl.fmTrapIntDisable = 1;         /* 1 = disable FM Trap interrupt */
    
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL, fmNmiCtrl.raw);

    acLinkCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL);
    acLinkCtrl.fmPcmDataOutput = false;     /* Disable FM PCM stream playback via AC-Link */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL, acLinkCtrl.raw);
}

static void vfm_tsrSetupGlobals(pci_Device dev) {
    /* Set up globals for us and our epical ASM core (tm) */
    g_vfm_pciDevice = dev;

    /* Get PCI I/O Base, FM NMI I/O Base and PCI IRQ */
    g_vfm_ioBaseDma = (u16) pci_read32(dev, V97_PCI_REG_IO_BASE_0) & 0xFFF0;
    g_vfm_ioBaseNmi = (u16) pci_read32(dev, V97_PCI_REG_IO_BASE_1) & 0xFFFC;
    g_vfm_pciIrq    = pci_read8(dev, V97_PCI_REG_IRQ_NUM);
}    

/* Initialize the memory for the DMA table and buffers, set up DMA table */
static void vfm_tsrSetupMemoryAndDma() {   
    u32 physAddr;
    u16 i;

    /* We cannot change segment alignments so we need to align the buffers based on the memory pool */
    u8 *alignedPtr =  g_vfm_fmDmaMemPool;
    alignedPtr += DMA_ALIGN - 1;
    alignedPtr -= ((u16) alignedPtr % DMA_ALIGN);
    physAddr = ((u32)FP_SEG(alignedPtr) << 4) + ((u32)FP_OFF(alignedPtr));

    /* Safety first :-) */
    memset(g_vfm_fmDmaMemPool, 0, VFM_MEM_POOL_SIZE);
 
    /* The first thing in the memory pool is the DMA table */
    g_vfm_fmDmaTable            = (v97_SgdTableEntry *) alignedPtr;
    g_vfm_fmDmaTablePhysAddress = physAddr;
        
    printf("[DMA Table    ] Base: %lp, Physical 0x%lx\n", (u8 far*) alignedPtr);

    /* Set up DMA table and buffer pointers, starting at the *end* of the DMA table */
    alignedPtr  +=       (sizeof(v97_SgdTableEntry) * DMA_BUFFER_COUNT);
    physAddr    += (u32) (sizeof(v97_SgdTableEntry) * DMA_BUFFER_COUNT);
    
    for (i = 0; i < DMA_BUFFER_COUNT; i++) {
        printf("[DMA Buffer %2u] Base: %lp, Physical 0x%08lx\n", (u8 far*) alignedPtr, physAddr);

        /* In our local pointer table */
        g_vfm_fmDmaBuffers[i] = (i16*) alignedPtr;
        
        /* In the sound chip's SGD DMA Table */
        g_vfm_fmDmaTable[i].baseAddress = physAddr;
        g_vfm_fmDmaTable[i].countFlags.stop = 0;
        
        /* If this is the final buffer, mark it as EOL, else FLAG */
        if (DMA_BUFFER_COUNT == (i + 1)) {
            g_vfm_fmDmaTable[i].countFlags.flag = 0;
            g_vfm_fmDmaTable[i].countFlags.eol  = 1;
        } else {
            g_vfm_fmDmaTable[i].countFlags.flag = 1;
            g_vfm_fmDmaTable[i].countFlags.eol  = 0;
        }

        physAddr += (u32) FM_PCM_BUFFER_ALLOC_SIZE;
        alignedPtr += FM_PCM_BUFFER_ALLOC_SIZE;
    }
    /* Tell audio device the pointer table address */
    sys_outPortL(g_vfm_ioBaseDma + V97_FM_SGD_TABLE_PTR, g_vfm_fmDmaTablePhysAddress);   
}

void vfm_tsrInitialize(pci_Device dev) {
    vfm_tsrSetupGlobals(dev);

    printf("[TSR Init     ] I/O Port (DMA): 0x%04x, I/O Port (NMI): 0x%04x\n", g_vfm_ioBaseDma, g_vfm_ioBaseNmi);

    vfm_tsrSetupMemoryAndDma();

    OPL3_Reset(&g_vfm_oplChip, FM_PCM_SAMPLE_RATE); /* Init OPL3 Emulator */
    vfm_tsrSetupInterrupts();                       /* Set up vectors and PIC for our interrupts */
    vfm_tsrSetupPCIRegisters();                     /* Set up PCI registers for playback & DMA */
    vfm_tsrStartDma();                              /* Start DMA */
    
    printf("[TSR Init Done]\n");
}

void vfm_tsrCleanup() {
    vfm_tsrStopDma();
    vfm_tsrUnsetPCIRegisters();
    vfm_tsrRestoreInterrupts();
}

void vfm_terminateAndStayResident() {
    /* 
    TODO 
    _dos_keep(0, (vfm_getTsrSize()+15) >> 4);
    */
    
}

void vfm_tsrUnload() {
    /*
    TODO
    */
}

bool vfm_tsrHasIntOccurred() {
    u8 flag = g_DMA_IRQOccured;
    
    /* Reset the flag, 1 will be returned to caller */
    g_DMA_IRQOccured = 0;
    
    return flag == 1;
}

i16 *vfm_tsrGetDmaBufferWritePtr() {
    return g_vfm_fmDmaBuffers[g_DMA_BufferIndex];
}

u16 vfm_tsrGetDmaBufferSize() {
    return FM_PCM_BUFFER_ALLOC_SIZE;
}

u16 vfm_tsrGetNmiHandlerSize() {
    u32 startPhys = ((u32)FP_SEG(vfm_nmiHandler) << 4) + ((u32)FP_OFF(vfm_nmiHandler));
    u32 endPhys   = ((u32)FP_SEG(vfm_nmiHandlerEnd) << 4) + ((u32)FP_OFF(vfm_nmiHandlerEnd));
    return (u16) (startPhys - endPhys);
}

u16 vfm_tsrGetTsrSize(void) {
    return (u16) vfm_tsrGetTsrSize;
}

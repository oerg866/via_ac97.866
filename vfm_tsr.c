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
#include "sys.h"
#include "pci.h"

#include <stdlib.h>
#include <stddef.h>
#include <dos.h>
#include <malloc.h>
#include <string.h>

#define UNUSED_ARG(_arg_) (void) _arg_

typedef void (_interrupt _far *IRQHANDLER)(void);

#define FM_PCM_SAMPLE_RATE 24000
#define STEREO 2
#define DMA_ALIGN 8
#define FM_PCM_BUFFER_ALLOC_SIZE (sizeof(i16) * SAMPS_PER_BUF * STEREO)
#define VFM_MEM_POOL_SIZE (sizeof(v97_SgdTableEntry) * NUM_BUFS + FM_PCM_BUFFER_ALLOC_SIZE * NUM_BUFS + DMA_ALIGN)

/*  Static memory pools for DMA table and FM DMA buffers
    The reason we do it this way is because traditional "code models" in DOS don't support arbitrary data alignments
    so we allocate a memory pool larger than what's needed and then programatically set the pointers */

u8                                  g_vfm_fmDmaMemPool[VFM_MEM_POOL_SIZE] = { 0 };  /* Memory pool for SGD Table + Buffers + Alignmnet reserve */
v97_SgdTableEntry                  *g_vfm_fmDmaTable = NULL;                /* Pointer to SGD Table */
u32                                 g_vfm_fmDmaTablePhysAddress = 0;        /* Physical 32-Bit address of SGD table */
i16                                *g_vfm_fmDmaBuffers[NUM_BUFS] = { 0 };   /* Pointer list for all DMA buffers */

pci_Device                          g_vfm_pciDevice     = { 0 };            /* PCI audio device structure (bus, slot, function) */
u16                                 g_vfm_pciIrq        = 0;                /* Interrupt of the PCI Audio Device */
bool                                g_vfm_slaveIrq      = false;            /* Device is on master/slave PIC (true if IRQ > 7) */
u16                                 g_vfm_ioBaseDma     = 0;                /* Base I/O port for Audio Codec / SGD Interface */
u16                                 g_vfm_ioBaseNmi     = 0;                /* Base I/O port for FM NMI Status / Data */
vfm_VirtualDmaDescriptor            g_vfm_vdsDescriptor = { 0 };            /* VDS Descriptor for Virtual DMA services */
bool                                g_vfm_vdsUsed       = false;            /* Flag indicating that VDS is used in this session */

/* Definitions from vfm_isr.asm */
extern u8                           g_DMA_IRQOccured;                       /* Flag by ISR when device IRQ has occured *and* was handled by us */
extern u8                           g_DMA_BufferIndex;                      /* Buffer Index currently used by DMA engine for writing */

/* These are far objects as they reside in cs, not ds! */
extern IRQHANDLER _far              g_vfm_oldPciIsr;                        /* Previous Interrupt Handler for device IRQ */
extern IRQHANDLER _far              g_vfm_oldNmiIsr;                        /* Previous Interrupt Handler for NMI */


extern void _far _interrupt         vfm_dmaInterruptHandler(void);          /* Our ISR for device IRQ */
extern void _far _interrupt         vfm_nmiHandler(void);                   /* Our ISR for NMI */
extern void _far                    vfm_nmiHandlerEnd(void);                /* Dummy function for a pointer to end of the NMI */

#ifndef DBOPL
#include "nukedopl/opl3.h"
opl3_chip                           g_vfm_oplChip;
#define vfm_oplInit()               OPL3_Reset(&g_vfm_oplChip, FM_PCM_SAMPLE_RATE)
#define vfm_oplGenOne(buf)          OPL3_Generate2ChResampled(&g_vfm_oplChip, buf)
#define vfm_oplGen(buf, samps)      OPL3_GenerateStream(&g_vfm_oplChip, buf, samps)
#define vfm_oplReg(reg, val)        OPL3_WriteReg(&g_vfm_oplChip, reg, val)
#else
#include "dbopl/dbopl.h"
Chip                                g_vfm_oplChip;
#define vfm_oplInit()               Chip_Reset(&g_vfm_oplChip, true, FM_PCM_SAMPLE_RATE)
#define vfm_oplGenOne(buf)          Chip_Generate(&g_vfm_oplChip, buf, 1)
#define vfm_oplGen(buf, samps)      Chip_Generate(&g_vfm_oplChip, buf, samps)
#define vfm_oplReg(reg, val)        Chip_WriteReg(&g_vfm_oplChip, reg, val)
#endif

/* Get physical address for a FAR PTR - kinda hacky */
static u32 vfm_tsrGetPhysAddr(void _far *ptr) {
    u32 segment = (u32) FP_SEG(ptr);
    u32 offset = (u32) FP_OFF(ptr);
    u32 ret = (segment << 4) + offset;

    DBG_PRINT("vfm_tsrGetPhysAddr: %lp (%04lx:%04lx) -> %08lx\n",
        (void _far*) ptr,
        segment, offset,
        ret);

    return ret;
}

/* Get Legacy Sound Blaster Port from PCI registers */
static u16 vfm_getSoundBlasterPort(pci_Device dev) {
    u8 sbEnabledBit = 0x01 & pci_read8(g_vfm_pciDevice, V97_PCI_REG_FUNC_ENABLE);
    u8 sbPortBits   = 0x03 & pci_read8(g_vfm_pciDevice, V97_PCI_REG_PNP_CTRL);

    if (!sbEnabledBit) return 0;

    return 0x220 + (sbPortBits * 0x20);
}

/* Write to Sound Blaster Mixer */
static void vfm_tsrSbMixerWrite(u16 sbPort, u8 reg, u8 val) {
    sys_outPortB(sbPort+4, reg); sys_ioDelay(3);
    sys_outPortB(sbPort+5, val); sys_ioDelay(3);
}

/* Initialize Sound Blaster Mixer to unmute FM audio */
static bool vfm_tsrSbMixerInit(pci_Device dev) {
    u16 sbPort = vfm_getSoundBlasterPort(dev);
    u16 resetPort = sbPort + 6;
    u16 dspIoPort = sbPort + 0x0C;
    u16 readPort  = sbPort + 0x0A;
    i16 timeout = 1000;

    DBG_PRINT("SB Port: %03x\n", sbPort);

    /* If port fetching failed, get out */
    if (sbPort == 0) {
        return false;
    }

    /* Reset SB DSP */
    sys_outPortB(resetPort, 1); sys_ioDelay(3);
    sys_outPortB(resetPort, 0); sys_ioDelay(3);

    while (timeout--) {
        if(sys_inPortB(readPort) == 0xAA)
            break;

        sys_ioDelay(1);
    }
    
    if (timeout == 0) {
        vfm_puts("ERROR: SB Not responding!\n");
        return false;
    }

    /* Enable Speaker */
    sys_outPortB(dspIoPort, 0xD1);

    vfm_tsrSbMixerWrite(sbPort, 0x22, 0xFF); /* Master Volume */
    vfm_tsrSbMixerWrite(sbPort, 0x26, 0xFF); /* FM Volume */
    vfm_tsrSbMixerWrite(sbPort, 0x28, 0xFF); /* CD Audio Volume */
    vfm_tsrSbMixerWrite(sbPort, 0x2E, 0xFF); /* Line In Volume */

    return true;
}

/* START FM SGD DMA playback stream */
static void vfm_tsrStartDma() {
    v97_SgdChannelType sgdType;
    v97_SgdChannelCtrl sgdCtrl;

    /* Tell audio device the pointer table address */
    sys_outPortL(g_vfm_ioBaseDma + V97_FM_SGD_TABLE_PTR, g_vfm_fmDmaTablePhysAddress);
    sys_ioDelay(1000);
    DBG_PRINT("%04x: %08lx (expect %08lx) \n", g_vfm_ioBaseDma, sys_inPortL(g_vfm_ioBaseDma + V97_FM_SGD_TABLE_PTR), g_vfm_fmDmaTablePhysAddress);

    sgdType.raw = 0;
    sgdType.intOnFlag = 1;          /* PCI Interrupt on FLAG */
    sgdType.intOnEOL = 1;           /* PCI Interrupt on EOL */
    sgdType.intSelect = 0;          /* Interrupt at PCI Read of Last Line */
    sgdType.autoStartSgdAtEOL = 1;  /* Auto-Restart DMA when last block is played (EOL) */
    sys_outPortB(g_vfm_ioBaseDma + V97_FM_SGD_TYPE, sgdType.raw);
    
    sgdCtrl.raw = 0;
    sgdCtrl.start = 1;              /* SGD Start */
    sys_outPortB(g_vfm_ioBaseDma + V97_FM_SGD_CTRL, sgdCtrl.raw);

    sys_ioDelay(1000);

    vfm_tsrPrintStatus(false);
}

/* STOP FM SGD DMA playback stream */
static void vfm_tsrStopDma() {
    v97_SgdChannelType sgdType;
    v97_SgdChannelCtrl sgdCtrl;

    sgdType.raw = 0;
    sys_outPortB(g_vfm_ioBaseDma + V97_FM_SGD_TYPE, sgdType.raw);
    
    sgdCtrl.raw = 0;
    sgdCtrl.terminate = 1;
    sys_outPortB(g_vfm_ioBaseDma + V97_FM_SGD_CTRL, sgdCtrl.raw);

    sys_ioDelay(1000);
}

/* Sets up device interrupt vector & mask as well as NMI vector */
static void vfm_tsrSetupInterrupts() {
#if 0
    u8  mask = ~(1 << (g_vfm_pciIrq & 0x07));
    u16 vector;
    u16 imrPort;

    if (g_vfm_pciIrq > 7) {
        vector  = g_vfm_pciIrq + 0x68;
        imrPort = 0xA1;
        g_vfm_slaveIrq = true;
    } else {
        vector  = g_vfm_pciIrq + 8;
        imrPort = 0x21;
    }
#endif 
    u8 irq = g_vfm_pciIrq;
    u16 vector = irq;
    u16 imrPort;
    u8  mask;

    if (irq > 7) {
        g_vfm_slaveIrq = true;
        irq -= 8;
        vector += 0x70 - 8;
        imrPort = 0xA1;        
    } else {
        vector += 8;
        imrPort = 0x21;
    }


    /* Get old vectors & set new ones */
    g_vfm_oldPciIsr = (IRQHANDLER) _dos_getvect(vector);
    g_vfm_oldNmiIsr = (IRQHANDLER) _dos_getvect(0x02);

    _dos_setvect(vector, vfm_dmaInterruptHandler);
    _dos_setvect(0x02,   vfm_nmiHandler);

    /* Unmask IRQ in Interrupt Mask Register */
//    sys_outPortB(imrPort, mask & sys_inPortB(imrPort));
    
    /* Unmask IRQ in IMR */
    mask  = sys_inPortB(imrPort);
    mask &= ~(1 << irq);
    sys_outPortB(imrPort, mask);    
    
    DBG_PRINT("[DMA IRQ %2u ] IMR %02x, Vector 0x%02x, ISR @ %lp, Chain @ %lp\n", g_vfm_pciIrq, mask, vector, vfm_dmaInterruptHandler, g_vfm_oldPciIsr);
    DBG_PRINT("[NMI Handler]         Vector 0x02, ISR @ %lp, Chain @ %lp\n", vfm_nmiHandler, g_vfm_oldNmiIsr);
    DBG_PRINT("                      Vector from MSDOS: %lp\n", _dos_getvect(0x02));
    DBG_PRINT("                      g_vfm_oldPciIsr: %lp\n",&g_vfm_oldNmiIsr);
}

/* Restores device interrupt vector & mask as well as NMI vector */
static void vfm_tsrRestoreInterrupts() {
    u8  mask = 1 << (g_vfm_pciIrq & 0x07);
    u16 vector;
    u16 imrPort;

    if (g_vfm_pciIrq > 7) {
        vector  = g_vfm_pciIrq + 0x68;
        imrPort = 0xA1;
    } else {
        vector  = g_vfm_pciIrq + 8;
        imrPort = 0x21;
    }

    /* Mask IRQ in Interrupt Mask Register */

    mask = mask | sys_inPortB(imrPort);
    sys_outPortB(imrPort, mask);

    /* Restore old ISRs */
    _dos_setvect(vector, g_vfm_oldPciIsr);
    _dos_setvect(0x02,   g_vfm_oldNmiIsr);
}

/*  Sets PCI registers so that we can... work :P */
static void vfm_tsrSetupPCIRegisters() {
    v97_FmNmiCtrl   fmNmiCtrl;  /* FM NMI Control Register */
    v97_ACLinkCtrl  acLinkCtrl; /* AC-Link Control Register */

    fmNmiCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL);
    fmNmiCtrl.fmSgdDataForSbMixing = 1;  /* FM PCM stream responds to SB mixer settings */
    fmNmiCtrl.fmIrqSelect = 0;              /* NMI, not SMI - TODO: Make this configurable? SMI may be more compatible */
    fmNmiCtrl.fmTrapIntDisable = 0;         /* 0 = enable FM Trap interrupt */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL, fmNmiCtrl.raw);

    acLinkCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL);
    acLinkCtrl.fmPcmDataOutput = 1;      /* Enable FM PCM stream playback via AC-Link */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL, acLinkCtrl.raw);
}

/*  Un-sets the PCI registers we set during init */
static void vfm_tsrUnsetPCIRegisters() {
    v97_FmNmiCtrl   fmNmiCtrl;  /* FM NMI Control Register */
    v97_ACLinkCtrl  acLinkCtrl; /* AC-Link Control Register */

    fmNmiCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL);
    
    fmNmiCtrl.fmSgdDataForSbMixing = 0; /* FM PCM stream doesn't respond to SB mixer settings */
    fmNmiCtrl.fmTrapIntDisable     = 1;  /* 1 = disable FM Trap interrupt */
    
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_FM_NMI_CTRL, fmNmiCtrl.raw);

    acLinkCtrl.raw = pci_read8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL);
    acLinkCtrl.fmPcmDataOutput = 0;     /* Disable FM PCM stream playback via AC-Link */
    pci_write8(g_vfm_pciDevice, V97_PCI_REG_AC_LINK_CTRL, acLinkCtrl.raw);
}

/* Set up globals for us and our epical ASM core (tm) */
static void vfm_tsrSetupGlobals(pci_Device dev) {
    u8 pciRevision = pci_read8(dev, V97_PCI_REG_REVISION);
    g_vfm_pciDevice = dev;

    /* Get PCI I/O Base, FM NMI I/O Base and PCI IRQ */
    g_vfm_ioBaseDma = (u16) pci_read32(dev, V97_PCI_REG_IO_BASE_0) & 0xFFF0;
    g_vfm_ioBaseNmi = (u16) pci_read32(dev, V97_PCI_REG_IO_BASE_1) & 0xFFFC;
    g_vfm_pciIrq    = pci_read8(dev, V97_PCI_REG_IRQ_NUM);

    /* In case of VT8231, the FM SGD Registers moved from I/O 2x to I/O 5x */
    DBG_PRINT("Chip revision: 0x%02x\n", pciRevision);

    if (pciRevision & 0xF0 == 0x40) {
        vfm_puts("VT8231 detected!\n\n");
        g_vfm_ioBaseDma += 0x0030;
    }
}    

/* Set up Virtual DMA Services (VDS) if available */
static bool vfm_setupVirtualDMA(u8 *ptr) {
    bool vdsSupported = vfm_vdsIsSupported();
    u8 _far *farPtr = (u8 _far*) ptr;

    if (!vdsSupported) {
        DBG_PRINT("VDS not supported, skipping...\n");
        return false;
    }

    g_vfm_vdsDescriptor.bufferId = 0;
    g_vfm_vdsDescriptor.physAddr = 0L;
    g_vfm_vdsDescriptor.segment = FP_SEG(farPtr);
    g_vfm_vdsDescriptor.offset = (u32) (FP_OFF(farPtr));
    g_vfm_vdsDescriptor.size = (u32) (VFM_MEM_POOL_SIZE - DMA_ALIGN);

    g_vfm_vdsUsed = vfm_vdsLockDmaRegion(&g_vfm_vdsDescriptor, 0x4); /* VIAFMTSR uses 4, not sure why */

    return g_vfm_vdsUsed;
}

/* Initialize the memory for the DMA table and buffers, set up DMA table */
static void vfm_tsrSetupMemoryAndDma() {   
    u32 physAddr;
    u16 i;

    /* We cannot change segment alignments so we need to align the buffers based on the memory pool */
    u8 *alignedPtr =  g_vfm_fmDmaMemPool;
    alignedPtr += DMA_ALIGN - 1;
    alignedPtr -= ((u16) alignedPtr % DMA_ALIGN);
    physAddr = vfm_tsrGetPhysAddr(alignedPtr);

    /* Attempt to set up virtual DMA region in case we're being LoadHigh'd */
    if (vfm_setupVirtualDMA(alignedPtr) && g_vfm_vdsDescriptor.physAddr != 0UL) {
        /* Override since physAddr may be remapped for HMA */
        physAddr = g_vfm_vdsDescriptor.physAddr;
    }

    /* Safety first :-) */
    g_DMA_IRQOccured = 0;
    g_DMA_BufferIndex = 0;
 
    /* The first thing in the memory pool is the DMA table */
    g_vfm_fmDmaTable            = (v97_SgdTableEntry *) alignedPtr;
    g_vfm_fmDmaTablePhysAddress = physAddr;

    DBG_PRINT("[DMA Table    ] Base: %lp, Physical 0x%08lx\n", (u8 far*) alignedPtr, physAddr);

    /* Set up DMA table and buffer pointers, starting at the *end* of the DMA table */
    alignedPtr  +=       (sizeof(v97_SgdTableEntry) * NUM_BUFS);
    physAddr    += (u32) (sizeof(v97_SgdTableEntry) * NUM_BUFS);

    for (i = 0; i < NUM_BUFS; i++) {
        /* In our local pointer table */
        g_vfm_fmDmaBuffers[i] = (i16*) alignedPtr;
        
        /* In the sound chip's SGD DMA Table */
        g_vfm_fmDmaTable[i].baseAddress = physAddr;
        g_vfm_fmDmaTable[i].countFlags.stop = 0;
        g_vfm_fmDmaTable[i].countFlags.length = (u32) FM_PCM_BUFFER_ALLOC_SIZE;
        
        /* If this is the final buffer, mark it as EOL, else FLAG */
        if (NUM_BUFS == (i + 1)) {
            g_vfm_fmDmaTable[i].countFlags.flag = 0;
            g_vfm_fmDmaTable[i].countFlags.eol  = 1;
        } else {
            g_vfm_fmDmaTable[i].countFlags.flag = 1;
            g_vfm_fmDmaTable[i].countFlags.eol  = 0;
        }

        DBG_PRINT("[DMA Buffer %2u] Base: %lp, Phys 0x%08lx, Length %lu, EOL %lu, FLAG %lu\n",
            i, 
            (void _far*) g_vfm_fmDmaBuffers[i],
            g_vfm_fmDmaTable[i].baseAddress,
            g_vfm_fmDmaTable[i].countFlags.length,
            g_vfm_fmDmaTable[i].countFlags.eol,
            g_vfm_fmDmaTable[i].countFlags.flag);

        alignedPtr  +=       FM_PCM_BUFFER_ALLOC_SIZE;
        physAddr    += (u32) FM_PCM_BUFFER_ALLOC_SIZE;
    }

}

bool vfm_tsrInitialize(pci_Device dev) {
    vfm_tsrSetupGlobals(dev);   vfm_puts("\xFE");
    
    DBG_PRINT("[TSR Init     ] I/O Port (DMA): 0x%04x, I/O Port (NMI): 0x%04x\n", g_vfm_ioBaseDma, g_vfm_ioBaseNmi);

    if (!vfm_tsrSbMixerInit(dev)) {                 /* Setup SB mixer to unmute FM */
        vfm_puts("ERROR: SB Audio not enabled - run VIA_AC97.EXE\n");
        return false;
    }

    vfm_puts("\xFE");

    vfm_tsrStopDma();           vfm_puts("\xFE");   /* Stop any previous DMA (shouldn't happen but you never know) */
    vfm_tsrSetupMemoryAndDma(); vfm_puts("\xFE");   /* Init DMA tables and buffers */
    vfm_oplInit();              vfm_puts("\xFE");   /* Init OPL3 Emulator */
    vfm_tsrSetupInterrupts();   vfm_puts("\xFE");   /* Set up vectors and PIC for our interrupts */
    vfm_tsrSetupPCIRegisters(); vfm_puts("\xFE");   /* Set up PCI registers for playback & DMA */ 
    vfm_tsrStartDma();          vfm_puts("\xFE");   /* Start DMA */

    vfm_puts("\n\nInit Complete\n");
}

void vfm_tsrCleanup() {
    vfm_tsrStopDma();
    vfm_tsrUnsetPCIRegisters();
    vfm_tsrRestoreInterrupts();

    /* If we had a VDS region locked, we need to unlocked */
    if (g_vfm_vdsUsed) {
        vfm_vdsUnlockDmaRegion(&g_vfm_vdsDescriptor);
    }
}

void vfm_tsrPrintStatus(bool cr) {
    u32 currentTablePtr = sys_inPortL(g_vfm_ioBaseDma + V97_FM_SGD_TABLE_PTR);
    u32 currentPos = sys_inPortL(g_vfm_ioBaseDma + V97_FM_SGD_CURRENT_POS);
    u16 currentStatus = (u16) sys_inPortB(g_vfm_ioBaseDma + V97_FM_SGD_STATUS);
    u16 ctrl = (u16) sys_inPortB(g_vfm_ioBaseDma + V97_FM_SGD_CTRL);
    u16 type = (u16) sys_inPortB(g_vfm_ioBaseDma + V97_FM_SGD_TYPE);

    DBG_PRINT("ioBase: %x TablePtr: %08lx Status: %02x %02x %02x Pos: %08lx",
        g_vfm_ioBaseDma,
        currentTablePtr,
        currentStatus,
        ctrl,
        type,
        currentPos);

    DBG_PRINT(cr ? "\r" : "\n");
}

#ifdef DBG_BENCH

#include <stdio.h>
#include <malloc.h>
#pragma pack(1)
typedef struct {
    u16 r; 
    u8 v;
} DBGREG;
#pragma pack()

static u32 vfm_tsrGetTicks() {
    u32 timer[2];
    u32 _far *_low = timer+0;
    u32 _far *_high = timer+1;
    u32 ret;
    _asm {
        PUSH32(_EAX)
        PUSH32(_EDX)
        RDTSC
        MOV_DWORDPTR_REG(_low, _EAX)
        MOV_DWORDPTR_REG(_high, _EDX)
        POP32(_EDX)
        POP32(_EAX)
    };

    //printf("%08lx%08lx\n", timer[1], timer[0]);
    ret = (timer[1] << 20UL) & 0xFFF00000UL  |  timer[0] >> 12UL;
    return ret;
}

void vfm_tsrOplTest() {
    FILE *f = fopen("teraterm.log", "rb");
    FILE *out = fopen("stream.bin", "wb");
    DBGREG reg;
    i16 *stream = malloc(512 * STEREO * sizeof(i16));
    i16 *streamOut = stream;
    u16 block = 0;
    vfm_oplInit();

    printf("OPL Init done\n");

    while (!feof(f) && !kbhit()) {
        i16 *streamOut = stream;
        u32 toWrite = 512ul;
        u32 startTime = vfm_tsrGetTicks();
        u32 elapsed = 0;

        while (1) {
            fread(&reg, 3, 1, f);
            if (ferror(f) || ferror(out)) abort();
            if (reg.r == 0xffff && reg.v == 0xff) break;
            
	        vfm_oplReg(reg.r, reg.v);
            vfm_oplGenOne(streamOut);
            streamOut += STEREO;
            toWrite--;
        }

        vfm_oplGen(streamOut, toWrite);

//        while (toWrite) {
//            vfm_oplGen(streamOut, toWrite);
//            streamOut += STEREO;
//            toWrite--;
//        }

//        vfm_oplGen(streamOut, toWrite);
        elapsed = vfm_tsrGetTicks() - startTime;

        printf("block %5u %5lu ticks \r", block++, elapsed);
        fwrite(stream, 512 * STEREO * sizeof(i16), 1, out);
    }
    printf("\n\n");
    free(stream);
    fclose(f);
    fclose(out);
}

#endif

void vfm_terminateAndStayResident() {
    u16 paras;
    u16 _psp;   /* pspspspspspspspsps :3 */
    u16 _ss;
    u16 _sp;

   _asm {
        push es
        mov ah, 0x51
        int 0x21            /* Get PSP segment in bx */
        mov _psp, bx
        mov es, es:[0x2C]   /* Get environment block */
        mov ah, 0x49
        int 0x21            /* Deallocate */
        pop es

        mov _ss, ss         /* Save Stack segment and pointer for size calculation */
        mov _sp, sp
    }

    /*  Calculation from A to Z of DOS Programming, Chapter 27 'TSR PROGRAMMING'.
        I have no idea if this is correct. But it seems? to work? maybe? */

    paras = _ss + (_sp >> 4) - _psp + 0x100; /* I kept adding to this value until it stopped crashing... Eto bleh...*/

    DBG_PRINT("psp %04x, ss %04x, sp %04x, paragraphs: %u\n", _psp, _ss, _sp, paras);

    _dos_keep(0, paras);
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

i16 *vfm_tsrGetDmaBufferWritePtr(i16 index) {
    /* Negative index = get current buffer write pointer */
    if (index < 0) {
        return g_vfm_fmDmaBuffers[g_DMA_BufferIndex];
    }

    return g_vfm_fmDmaBuffers[index];
}

u16 vfm_tsrGetDmaBufferSize() {
    return FM_PCM_BUFFER_ALLOC_SIZE;
}

u16 vfm_tsrGetDmaBufferCount() {
    return NUM_BUFS;
}

u16 vfm_tsrGetNmiHandlerSize() {
    u32 startPhys = vfm_tsrGetPhysAddr(vfm_nmiHandler);
    u32 endPhys   = vfm_tsrGetPhysAddr(vfm_nmiHandlerEnd);
    return (u16) (endPhys - startPhys);
}

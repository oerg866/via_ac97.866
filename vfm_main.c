/* VIA_AC97.866 FM Emulation TSR
 * 
 * (C) 2025 Eric Voirin (Oerg866)
 *
 * LICENSE: CC-BY-NC-SA 4.0
 *
 * VIA VT82C686x AC97 Hardware-Assisted FM Emulation Driver TSR for DOS
 */

#include <stdlib.h>
#include <dos.h>
#include <string.h>
#include <malloc.h>
#include <conio.h>
#include <fcntl.h>

#include "types.h"
#include "pci.h"
#include "sys.h"

#include "vfm_tsr.h"
#include "v97_reg.h"

static void oplWrite(u8 reg, u8 val) {
//    DBG_PRINT("OPL Write: %02x %02x\n", reg, val);
    sys_outPortB(0x388, reg);
    sys_ioDelay(40);
    sys_outPortB(0x389, val);
//    sys_ioDelay(230);
}

static void oplWriteNoReg(u8 val) {
    sys_outPortB(0x389, val);
//    sys_ioDelay(230);
}

/* Plays a basic sine wave on the OPL's first channel */
static void vfm_fmGenerateTestTone() {
    /* Turn off key-on before reconfiguring */
    oplWrite(0xB0 + 0, 0x00);

    /* 0x20 + operator = set AM/VIB/EG-TYPE/KSR/MULT bits */
    oplWrite(0x20 + 0, 0x21);
    oplWrite(0x20 + 3, 0x21);

    /* 0x40 + operator => KSL, total level */
    oplWrite(0x40 + 0, 0x3F);
    oplWrite(0x40 + 3, 0x10);

    /* 0x60 + operator  = Attack/Decay */
    oplWrite(0x60 + 0, 0xF9);  /* mod */
    oplWrite(0x60 + 3, 0xF9);  /* car */

    /* 0x80 + operator = Sustain/Release*/
    oplWrite(0x80 + 0, 0x09);
    oplWrite(0x80 + 3, 0x09);

    /* 0xC0 + channel = Feedback/Connection */
    oplWrite(0xC0 + 0, 0x00);

    /* 0xA0 + channel (low 8 bits) and 0xB0 + channel (high 2 bits plus keyon) = Frequency */
    oplWrite(0xA0 + 0, 0xFF);
    oplWrite(0xB0 + 0, 0x2F);
}

/* Plays a test tone and waits for a keypress before quitting. */
static void vfm_testToneLoopTest() {
    bool cancel = false;

    vfm_fmGenerateTestTone();

    vfm_puts("<Key to quit>\n");

    while (!kbhit() && !cancel) {
        while (!cancel && !vfm_tsrHasIntOccurred()) {
            /* Crickets */
            sys_ioDelay(1);
            vfm_tsrPrintStatus(true);
            if (kbhit()) cancel = true;  
        }
    }

    /* Clear keyon */
   oplWrite(0xB0 + 0, 0x04);
}

#ifdef DBG_FILE
/* Plays a Signed 16-Bit Stereo RAW PCM file on the FM PCM channel */
static void vfm_fileLoopTest() {
    int fileHandle  = 0;
    u16 bufferSize = vfm_tsrGetDmaBufferSize();

    if (0 != _dos_open("test.snd", O_RDONLY, &fileHandle)) {
        vfm_puts("file open error\n");
        return;
    }

    while (!kbhit()) {
        bool cancel = false;
        u16 bytesRead = 0;
        i16 *dmaBuffer;

        while (!cancel && !vfm_tsrHasIntOccurred()) {
            /* Crickets */
            sys_ioDelay(1);
            if (kbhit()) cancel = true;
        }

        if (cancel) break;

        dmaBuffer = vfm_tsrGetDmaBufferWritePtr(-1);
        
        /* Read next chunk from file */

        _dos_read(fileHandle, dmaBuffer, bufferSize, &bytesRead);
        
        if (bytesRead == 0) {
            vfm_puts("End of file!\n");
            break;
        }

        /* If there were less bytes read, pad the output */
        if (bytesRead < bufferSize) {
            memset(dmaBuffer + bytesRead, 0, bufferSize - bytesRead);
        }

        vfm_tsrPrintStatus(true);
    }

    _dos_close(fileHandle);
}
#endif

/* Attempts to find signature of NMI handler in the code pointed to by the NMI vector. */
static bool vfm_isTsrLoaded() {
    u32 nmiHandlerSize = vfm_tsrGetNmiHandlerSize();
    
    u8 _far *nmiFunctionData = (u8 _far *) _dos_getvect(0x02);
    u8 _far *nmiFunctionDataEnd = nmiFunctionData + nmiHandlerSize;

    u32 signature[] = { VFM_TSR_SIGNATURE_1, VFM_TSR_SIGNATURE_2 };
    u16 signatureSize = sizeof(signature);

    /*  NMI vector doesn't exist, clear case
        actually this shouldn't happen, I think, as DOS has a default handler */
    if (nmiFunctionData == NULL) return false;

    DBG_PRINT("nmiFunctionData: %lp %u\n", nmiFunctionData, nmiHandlerSize);

    while (nmiFunctionData + signatureSize <= nmiFunctionDataEnd) {
        if (0 == _fmemcmp(nmiFunctionData, signature, signatureSize)) {
            DBG_PRINT("%lp %u %08lx %08lx\n", nmiFunctionData, signatureSize, *((u32 _far*) nmiFunctionData), signature[0]);
            return true;
        }
        nmiFunctionData++;
    }

    return false;
}

#ifdef DBG_BUFFER
static void vfm_dumpDmaBuffers(const char *filename) {
    FILE *f = fopen(filename, "wb");
    u16 i;

    for (i = 0; i < vfm_tsrGetDmaBufferCount(); i++) {
        fwrite(vfm_tsrGetDmaBufferWritePtr(i), vfm_tsrGetDmaBufferSize(),  1, f);
    }

    fclose(f);
}
#endif

static void printUsage() {
    vfm_puts("r   Load TSR\n");
    vfm_puts("<for debugging only:>\n");
    vfm_puts("g   Init, play test tone and wait for key press\n");
    vfm_puts("p   Sends a test tone to OPL (no hw init)\n");
#ifdef DBG_FILE
    vfm_puts("f   Plays raw PCM S16LE file (test.snd)\n");
#endif

}

int main(int argc, char *argv[]) {
    pci_Device  dev;
    bool        tsrIsLoaded = vfm_isTsrLoaded();

    vfm_puts("VIA_AC97.866     - VIA AC'97 FM Emulation TSR Version 0.5\n");
    vfm_puts("                   (C) 2025      Eric Voirin (oerg866)\n");
#if defined(NUKED)
    vfm_puts("Nuked-OPL3       - (C) 2013-2020 Nuke.YKT\n");
#elif defined (DBOPL)
    vfm_puts("DBOPL            - (C) 2002-2021 The DOSBox Team\n");
#endif
    vfm_puts("\n");

    if (argc < 2) {
        printUsage();
        return 1;
    }

    /* check if program should send a test tone to OPL (not necessarily our device) */
    if (argc > 1 && *argv[1] == 'p') {
        vfm_fmGenerateTestTone();
        vfm_puts("OPL test tone sent\n");
        return 0;
    }

#ifdef DBG_BENCH
    /* check if program should do a OPL3 generation test & benchmark */
    if (argc > 1 && *argv[1] == 'o') {
        vfm_tsrOplTest();
        return 0;
    }
#endif

    /* Abort if the TSR is already loaded */
    if (tsrIsLoaded) {
        vfm_puts("The TSR is already loaded. Aborting...\n");
        return -1;
    }

    /* Check if PCI bus is accessible on this machine */
    if (!pci_test()) {
        vfm_puts("PCI Bus Error\n");
        return -1;
    }

    /* Check for VIA 686x Audio Device on the bus */
    if (!pci_findDevByID(0x1106, 0x3058, &dev)) {
        vfm_puts("Audio Device not found\n");
        return -1;
    }

    /* Set up the TSR's specific stuff - SB Mixer, Interrupt vectors, DMA tables, buffers */
    if (!vfm_tsrInitialize(dev)) {
        vfm_puts("Aborting...\n");
        return -1;
    }
    
    /* check if program should be loaded as TSR */
    if (argc > 1 && *argv[1] == 'r') {
        _dos_keep(0, 64000U >> 4);
    }

    /* check if program should do a basic fm generation test */
    if (argc > 1 && *argv[1] == 'g') {
        vfm_testToneLoopTest();

#ifdef DBG_BUFFER
        vfm_dumpDmaBuffers("dump.bin");
#endif

    }
    
#ifdef DBG_FILE
    /* check if program should do a file playback test */
    if (argc > 1 && *argv[1] == 'f') {
        vfm_fileLoopTest();
    }
#endif

    /* Done! :D */
    vfm_puts("[Finished]\n");

    vfm_tsrCleanup();
    return 0;
}

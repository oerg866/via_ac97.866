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
#include <stdio.h>
#include <malloc.h>
#include <conio.h>
#include <fcntl.h>

#include "types.h"
#include "pci.h"
#include "sys.h"

#include "vfm_tsr.h"
#include "v97_reg.h"

/*
static void vfm_printStatus(bool cr) {
    u16 ioBase = s_vfm_ioBase;
    printf("ioBase: %x TablePtr: %08lx Status: %02x Pos: %08lx",
        ioBase,
        sys_inPortL(ioBase + V97_FM_SGD_TABLE_PTR),
        inp(ioBase + V97_FM_SGD_STATUS),
        sys_inPortL(ioBase + V97_FM_SGD_CURRENT_POS));

    printf(cr ? "\r" : "\n");
}
*/

static void vfm_sbMixerWrite(u16 sbPort, u8 reg, u8 val) {
    outp(sbPort+4, reg); sys_ioDelay(3);
    outp(sbPort+5, val); sys_ioDelay(3);
}

static bool vfm_sbMixerInit(u16 sbPort) {
    u16 resetPort = sbPort + 6;
    u16 dspIoPort = sbPort + 0x0C;
    u16 readPort  = sbPort + 0x0A;
    i16 timeout = 1000;

    /* Reset SB DSP */
    outp(resetPort, 1); sys_ioDelay(3);
    outp(resetPort, 0); sys_ioDelay(3);


    while (timeout--) {
        if(inp(readPort) == 0xAA) {
            printf("SB Mixer Init OK\n");
            break;
        }
        sys_ioDelay(1);
    }
    
    if (timeout == 0) {
        printf("SB MIxer Initialization FAILED!\n");
        return false;
    }

    /* Enable Speaker */
    outp(dspIoPort, 0xD1);

    vfm_sbMixerWrite(sbPort, 0x22, 0xFF); /* Master Volume */
    vfm_sbMixerWrite(sbPort, 0x26, 0xFF); /* FM Volume */
    vfm_sbMixerWrite(sbPort, 0x28, 0xFF); /* CD Audio Volume */
    vfm_sbMixerWrite(sbPort, 0x2E, 0xFF); /* Line In Volume */
}

static void oplWrite(u8 reg, u8 val) {
//    printf("OPL Write: %02x %02x\n", reg, val);
    outp(0x388, reg);
    sys_ioDelay(40);
    outp(0x389, val);
//    sys_ioDelay(230);
}

static void oplWriteNoReg(u8 val) {
    outp(0x389, val);
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

    while (!kbhit() && !cancel) {
        while (!cancel && !vfm_tsrHasIntOccurred()) {
            /* Crickets */
            sys_ioDelay(1);
            if (kbhit()) cancel = true;  
        }
//        vfm_printStatus(true);
    }

    /* Clear keyon */
   oplWrite(0xB0 + 0, 0x04);
}

/* Plays a Signed 16-Bit Stereo RAW PCM file on the FM PCM channel */
void vfm_fileLoopTest() {
    int fileHandle  = 0;
    u16 bufferSize = vfm_tsrGetDmaBufferSize();

    if (0 != _dos_open("test.snd", O_RDONLY, &fileHandle)) {
        printf("file open error\n");
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

        dmaBuffer = vfm_tsrGetDmaBufferWritePtr();
        
        /* Read next chunk from file */

        _dos_read(fileHandle, dmaBuffer, bufferSize, &bytesRead);
        
        if (bytesRead == 0) {
            printf("End of file!\n");
            break;
        }

        /* If there were less bytes read, pad the output */
        if (bytesRead < bufferSize) {
            memset(dmaBuffer + bytesRead, 0, bufferSize - bytesRead);
        }

//        vfm_printStatus(true);
    }

    _dos_close(fileHandle);
}

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

    while (nmiFunctionData + signatureSize <= nmiFunctionDataEnd) {
        if (0 == _fmemcmp(nmiFunctionData, signature, signatureSize)) {
            return true;
        }
        nmiFunctionData++;
    }

    return false;
}

static void printUsage() {
    printf("r   Load TSR\n");
    printf("<for debugging only:>\n");
    printf("g   Init, play test tone and wait for key press\n");
    printf("f   Plays raw PCM S16LE file (test.snd)\n");
    printf("p   Sends a test tone to OPL (no hw init)\n");
}

int main(int argc, char *argv[]) {
    pci_Device  dev;
    bool        tsrIsLoaded = vfm_isTsrLoaded();

    printf("VIA_AC97.866     - VIA AC'97 FM Emulation TSR Version 0.1\n");
    printf("                   (C) 2025      Eric Voirin (oerg866)\n");
    printf("FM Emulation by:\n");
    printf("Nuked-OPL3       - (C) 2013-2020 Nuke.YKT\n");
    printf("---------------------------------------------------------\n");

    if (argc < 2) {
        printUsage();
        return 1;
    }

    /* check if program should be loaded as TSR */
    if (argc > 1 && *argv[1] == 'p') {
        vfm_fmGenerateTestTone();
        printf("OPL test tone sent\n");
        return 0;
    }

    /* Abort if the TSR is loaded */
    if (tsrIsLoaded) {
        printf("The TSR is already loaded. Aborting...\n");
        return -1;
    }

    /* Check if PCI bus is accessible on this machine */
    if (!pci_test()) {
        printf("PCI Bus Error\n");
        return -1;
    }

    /* Check for VIA 686x Audio Device on the bus */
    if (!pci_findDevByID(0x1106, 0x3058, &dev)) {
        printf("Audio Device not found\n");
        return -1;
    }

    /* Set up the TSR's specific stuff - Interrupt vectors, DMA tables, buffers */
    vfm_tsrInitialize(dev);

    /* Set up Sound Blaster mixer */
    vfm_sbMixerInit(0x220);
    
    /* check if program should be loaded as TSR */
    if (argc > 1 && *argv[1] == 'r') {
        _dos_keep(0, 64000U >> 4);
    }

    /* check if program should do a basic fm generation test */
    if (argc > 1 && *argv[1] == 'g') {
        vfm_testToneLoopTest();
    }
    
    /* check if program should do a file playback test */
    if (argc > 1 && *argv[1] == 'f') {
        vfm_fileLoopTest();
    }

    /* Done! :D */
    printf("[Finished]\n");

cleanup:
    vfm_tsrCleanup();
    return 0;
}

/*
 * VIA_AC97.866
 *
 * (C) 2025 Eric Voirin (Oerg866)
 *
 * LICENSE: CC-BY-NC 3.0
 *
 * VIA VT82C686x AC97 Initialization Driver for DOS
 *
 * Refer to README.MD
 */

#include <stdio.h>
#include <stdlib.h>
#include <dos.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
//#include <graph.h>

#include "types.h"
#include "pci.h"
#include "sys.h"

#define __LIB866D_TAG__ "V97_MAIN"
#include "debug.h"

#define VERSION "0.1"

typedef struct {
    u16 ioBase;
    u16 sbPort;
    u16 sbIrq;
    u16 sbDma;
    u16 midiPort;
    u16 joyPort;
    u32 codecId;
} v97_Cfg;

typedef struct {
    u8 sbEnable             :1;
    u8 midiEnable           :1;
    u8 fmEnable             :1;
    u8 joyEnable            :1;
    u8 sbpcmEmptyFifoGate   :1;
    u8 reg2CWritable        :1;
    u8 maskMidiIrq          :1;
    u8 midiPortCfgSwitch    :1;
} v97_FuncEnable;

typedef struct {
    u8 sbPort   : 2;
    u8 midiPort : 2;
    u8 sbDma    : 2;
    u8 sbIrq    : 2;
} v97_PnpCtrl;

typedef struct {
    u8  bits;
    u16 value;
} v97_BitsToU16;

static const v97_BitsToU16 c_v97_sbIrqs[] = {
    { 0x00, 5  },
    { 0x01, 7  },
    { 0x02, 9  },
    { 0x03, 10 },
};

static const v97_BitsToU16 c_v97_midiPorts[] = {
    { 0x00, 0x300 },
    { 0x01, 0x310 },
    { 0x02, 0x320 },
    { 0x03, 0x330 },
};

static const v97_BitsToU16 c_v97_sbPorts[] = {
    { 0x00, 0x220 },
    { 0x01, 0x240 },
    { 0x02, 0x260 },
    { 0x03, 0x280 },
};

static void v97_getLegacyConfig(pci_Device dev, v97_Cfg *cfg) {
    /* Get Legacy PnP Control register */
    v97_PnpCtrl pnpCtrl;
    
    pci_readBytes(dev, (u8*) &pnpCtrl, 0x43, 1);
    
    /* Funnel values from register bits */
    cfg->sbIrq      = c_v97_sbIrqs    [pnpCtrl.sbIrq].value;
    cfg->sbDma      = pnpCtrl.sbDma;
    cfg->midiPort   = c_v97_midiPorts [pnpCtrl.midiPort].value;
    cfg->sbPort     = c_v97_sbPorts   [pnpCtrl.sbPort].value;

    /* Get Joystick Port Base */
    cfg->joyPort = pci_read16(dev, 0x4A);
}

#define V97_READ            (1UL<<23UL)
#define V97_BUSY            (1UL<<24UL)
#define V97_PRIMARY_VALID   (1UL<<25UL)
#define V97_CMD_SHIFT       (16UL)
#define V97_DATA_SHIFT      (0UL)
#define V97_CODEC_IO_OFFSET (0x80)

#define AC97_REG_GENERAL        (0x20)
#define AC97_REG_GENERAL_3D_ON  (0x2000)
#define AC97_REG_PWR_STATUS     (0x26)
#define AC97_REG_VENDOR_ID1     (0x7C)
#define AC97_REG_VENDOR_ID2     (0x7E)

static void v97_codecWait(v97_Cfg *cfg) {
    u16 timeout = 1000;

    while (timeout--) {
        u32 status = sys_inPortL(cfg->ioBase + V97_CODEC_IO_OFFSET);
        
        /* codec not busy -> we're ok */
        if (0 == (status & V97_BUSY)) {
            return;
        }
        
        sys_ioDelay(1);
    }
    
    L866_ASSERTM(timeout, "Codec unresponsive!");
}

/* Read a register from the AC97 codec */
static u16 v97_codecRead(v97_Cfg *cfg, u16 reg) {
    u32 outVal  = ((u32) reg & 0x7FUL) << V97_CMD_SHIFT;
    outVal     |= V97_PRIMARY_VALID | V97_READ;
    
    sys_outPortL(cfg->ioBase + V97_CODEC_IO_OFFSET, outVal);
    sys_ioDelay(20);  
    v97_codecWait(cfg);
    sys_ioDelay(25);
    
    return 0xffffUL & sys_inPortL(cfg->ioBase + V97_CODEC_IO_OFFSET);
}

/* Write a register to the AC97 codec */
static void v97_codecWrite(v97_Cfg *cfg, u16 reg, u16 value) {
    u32 outVal  = ((u32) reg   & 0x7FUL) << V97_CMD_SHIFT;
    outVal     |= ((u32) value & 0x7FUL) << V97_DATA_SHIFT;

    sys_outPortL(cfg->ioBase + V97_CODEC_IO_OFFSET, outVal);
    v97_codecWait(cfg);
}


typedef enum {
    AC97_VOL_MASTER = 0x02,
    AC97_VOL_WAVE   = 0x04,
    AC97_VOL_PCSPK  = 0x0A,
    AC97_VOL_LINEIN = 0x10,
    AC97_VOL_CDIN   = 0x12,
    AC97_VOL_VIDEO  = 0x14,
    AC97_VOL_AUX    = 0x16,
    AC97_VOL_PCMOUT = 0x18,
} v97_VolumeCtrl;

static void v97_setVolume(v97_Cfg *cfg, v97_VolumeCtrl volCtrl, u16 left, u16 right, bool mute) {
    u16 regVal = (left & 0x7F) << 8 | right & 0x7F;
    
    if (mute) regVal |= 0x8000;

    v97_codecWrite(cfg, (u16) volCtrl, regVal);
    
    printf("Set AC97 Volume Register %02x - L %02x - R %02x - Mute: %s\n",
        (u16) volCtrl,
        left,
        right,
        mute ? "ON" : "OFF");
}

static bool v97_initCodec(pci_Device dev) {
    v97_Cfg cfg;
    v97_FuncEnable fe;
    pci_DeviceInfo devInfo;

    memset(&cfg, 0, sizeof(v97_Cfg));

    /* Get PCI device info (bars, etc) */
    pci_populateDeviceInfo(&devInfo, dev);

    cfg.ioBase = (u16) devInfo.bars[0].address;    
    
    /* Get Function Enable byte */
    pci_readBytes(dev, (u8*) &fe, 0x42, 1);
        
    /* Read current configuration */    
    v97_getLegacyConfig(dev, &cfg);
    
    /* Get AC97 Codec ID */
    cfg.codecId =  v97_codecRead(&cfg, AC97_REG_VENDOR_ID2);
    cfg.codecId <<= 16UL;
    cfg.codecId |= v97_codecRead(&cfg, AC97_REG_VENDOR_ID1);

    printf("AC97 IO Port:    0x%04x\n", cfg.ioBase);
    printf("AC97 Codec ID:   0x%08lx\n", cfg.codecId);
    printf("\n");
    printf("\n");
    printf("Function Enable: 0x%02x\n", *((u8*) &fe));
    printf("SoundBlaster:    %s\n", fe.sbEnable ? "Yes" : "No");
    printf("MIDI:            %s\n", fe.midiEnable ? "Yes" : "No");
    printf("FM Synth:        %s (Warning: requires VIAFMTSR)\n", fe.fmEnable ? "Yes" : "No");
    printf("Joystick Port:   %s\n", fe.joyEnable ? "Yes" : "No");
    printf("\n");
    printf("SB Pro Port 0x%03x, IRQ %u, DMA %u", cfg.sbPort, cfg.sbIrq, cfg.sbDma);
    printf("\n");
    
    /* Disable powerdown */
    v97_codecWrite(&cfg, AC97_REG_PWR_STATUS, 0x0000);
    v97_setVolume(&cfg, AC97_VOL_MASTER, 0, 0, false);
    v97_setVolume(&cfg, AC97_VOL_WAVE, 0, 0, false);
    v97_setVolume(&cfg, AC97_VOL_PCMOUT, 0, 0, false);
    v97_setVolume(&cfg, AC97_VOL_LINEIN, 0, 0, false);
    
    return false;
}

int main(int argc, char *argv[]) {
    pci_Device ac97dev;

    printf("\n");
    printf("VIA_AC97 Version %s\n", VERSION);
    printf("         (C)2025 E. Voirin (oerg866)\n");
    printf("         http://github.com/oerg866\n");
    printf("----------------------------------------\n");

    /* Check if PCI bus is accessible on this machine */

    if (!pci_test()) {
        printf("ERROR enumerating PCI bus. Quitting...\n");
        return -1;
    }

    /* Usage when parameters are insufficient */

#if 0
    if (argc < 3) {
        printf("TODO: Usage", argc);
        return -1;
    }
#endif

//    list();


    if (!pci_findDevByID(0x1106, 0x3058, &ac97dev)) {
        printf("VIA 686 Audio Device not found!\n");
        return -1;
    }
    
    printf("VIA 686 Audio Device found: %u %u %u\n", ac97dev.bus, ac97dev.slot, ac97dev.func);  


    /* */
    
    v97_initCodec(ac97dev);


    return 0;
}

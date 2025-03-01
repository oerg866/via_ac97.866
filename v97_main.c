/*
 * VIA_AC97.866
 *
 * (C) 2025 Eric Voirin (Oerg866)
 *
 * LICENSE: CC-BY-NC-SA 4.0
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

#include "types.h"
#include "pci.h"
#include "sys.h"
#include "util.h"
#include "ac97.h"
#include "args.h"

#define __LIB866D_TAG__ "V97_MAIN"
#include "debug.h"

/* VIA AC97 PCI / I/O interface defines */

#define V97_READ                    (1UL<<23UL)
#define V97_BUSY                    (1UL<<24UL)
#define V97_PRIMARY_VALID           (1UL<<25UL)
#define V97_CMD_SHIFT               (16UL)
#define V97_DATA_SHIFT              (0UL)
#define V97_CODEC_IO_OFFSET         (0x80)

/* Structure for register bits -> representative value look-ups */
typedef struct {
    u8  bits;
    u16 value;
} v97_RegBits;

/* This structure holds all the arguments passed to the program. */
typedef struct {
    struct {    bool setup;             /* User requests this feature to be setup */
                bool enable;
                u16  config[3]; } sb;   /* config = [address, irq, dma] */

    struct {    bool setup;             /* User requests this feature to be setup */
                bool enable;
                u16  port;      } midi;

    struct {    bool setup;             /* User requests this feature to be setup */
                bool enable;    } joy;

    struct {    bool setup;             /* User requests this feature to be setup */
                bool enable;    } fm;

    /* Hacky way to simplify volume mixer setup. */
    struct {    bool setup;
                u16  volume;    } volumes[__AC97_VOL_COUNT__];

    struct {    bool setup;
                bool enable;    } micBoost;

    struct {    bool setup;
                bool enable;    } surround;
} v97_AppConfig;

static v97_AppConfig v97_args;

/* Supported SB I/O Base Ports */
static const v97_RegBits  c_v97_sbPorts[] = {
    { 0x00, 0x220 },
    { 0x01, 0x240 },
    { 0x02, 0x260 },
    { 0x03, 0x280 },
};

/* Supported SB IRQ lines */
static const v97_RegBits c_v97_sbIrqs[] = {
    { 0x00, 5  },
    { 0x01, 7  },
    { 0x02, 9  },
    { 0x03, 10 },
};

/* Supported SB DMA channels */
static const v97_RegBits c_v97_sbDmas[] = {
    { 0x00, 0 },
    { 0x01, 1 },
    { 0x02, 2 },
    { 0x03, 3 },
};

/* Supported MPU401 I/O Ports */
static const v97_RegBits c_v97_midiPorts[] = {
    { 0x00, 0x300 },
    { 0x01, 0x310 },
    { 0x02, 0x320 },
    { 0x03, 0x330 },
};

/*  Looks up a Key in a v97_RegBits table, bitsKey is the output
    bitsKey can be NULL if it's just a "does n exist" operation */
static bool v97_regBitsLookup(const v97_RegBits *table, size_t entryCount, u16 valueTofind, u8 *bitsKey) {
    size_t i;
    for (i = 0; i < entryCount; i++) {
        if (table[i].value == valueTofind) {
            if (bitsKey) *bitsKey = table[i].bits;
            return true;
        }
    }
    return false;
}

/*  Looks up a Key in a v97_RegBits table, bitsKey is the output
    value can be NULL if it's just a "does n exist" operation */
static bool v97_regValueLookup(const v97_RegBits *table, size_t entryCount, u8 bitsKey, u16 *value) {
    size_t i;
    for (i = 0; i < entryCount; i++) {
        if (table[i].bits == bitsKey) {
            if (value) *value = table[i].value;
            return true;
        }
    }
    return false;
}

static void v97_codecWait(u16 ioBase) {
    u16 timeout = 1000;

    while (timeout--) {
        u32 status = sys_inPortL(ioBase + V97_CODEC_IO_OFFSET);
        
        /* codec not busy -> we're ok */
        if (0 == (status & V97_BUSY)) {
            return;
        }
        
        sys_ioDelay(1);
    }
    
    L866_ASSERTM(timeout, "Codec unresponsive!");
}

/* Read a register from the AC97 codec */
static u16 v97_codecRead(void *dev, u16 reg) {
    u16 ioBase  = *(u16*) dev;
    u32 outVal  = ((u32) reg & 0x7FUL) << V97_CMD_SHIFT;
    outVal     |= V97_PRIMARY_VALID | V97_READ;
    
    sys_outPortL(ioBase + V97_CODEC_IO_OFFSET, outVal);
    sys_ioDelay(20);  
    v97_codecWait(ioBase);
    sys_ioDelay(25);
    
    return 0xffffUL & sys_inPortL(ioBase + V97_CODEC_IO_OFFSET);
}

/* Write a register to the AC97 codec */
static void v97_codecWrite(void *dev, u16 reg, u16 value) {
    u16 ioBase  = *(u16*) dev;
    u32 outVal  = ((u32) reg   & 0x7FUL) << V97_CMD_SHIFT;
    outVal     |= ((u32) value)          << V97_DATA_SHIFT;

    sys_outPortL(ioBase + V97_CODEC_IO_OFFSET, outVal);
    v97_codecWait(ioBase);
}

/* Get 686 Audio Device AC Link Status Register */
static v97_ACLinkStatus v97_getACLinkStatusReg(pci_Device dev) {
    v97_ACLinkStatus reg;
    pci_readBytes(dev, (u8*) &reg, V97_PCI_REG_AC_LINK_STATUS, 1);
    return reg;
}

/* Get/set 686 Audio Device AC Link Interface Control Register */
static v97_ACLinkCtrl v97_getACLinkCtrlReg(pci_Device dev) {
    v97_ACLinkCtrl reg;
    pci_readBytes(dev, (u8*) &reg, V97_PCI_REG_AC_LINK_CTRL, 1);
    return reg;
}

static void v97_setACLinkCtrlReg(pci_Device dev, v97_ACLinkCtrl reg) {
    pci_writeBytes(dev, (u8*) &reg, V97_PCI_REG_AC_LINK_CTRL, 1);
}

/* Get/set 686 Audio Device Function Enable Register */
static v97_FuncEnable v97_getFuncEnableReg(pci_Device dev) {
    v97_FuncEnable reg;
    pci_readBytes(dev, (u8*) &reg, V97_PCI_REG_FUNC_ENABLE, 1);
    return reg;
}

static void v97_setFuncEnableReg(pci_Device dev, v97_FuncEnable reg) {
    pci_writeBytes(dev, (u8*) &reg, V97_PCI_REG_FUNC_ENABLE, 1);
}

/* Get/set 686 Audio Device PnP Control Register */
static v97_PnpCtrl v97_getPnpControlReg(pci_Device dev) {
    v97_PnpCtrl reg;
    pci_readBytes(dev, (u8*) &reg, V97_PCI_REG_PNP_CTRL, 1);
    return reg;
}

static void v97_setPnpControlReg(pci_Device dev, v97_PnpCtrl reg) {
    pci_writeBytes(dev, (u8*) &reg, V97_PCI_REG_PNP_CTRL, 1);
}

/* Bring up Southbridge to AC97 codec link */
static bool v97_enableACLink(pci_Device dev) {
    v97_ACLinkCtrl acLinkCtrl = v97_getACLinkCtrlReg(dev);
    u16 timeout = 1000;

    /* Don't do anything if it's already enabled, as it may be risky */
    if (acLinkCtrl.interfaceEnable && v97_getACLinkStatusReg(dev).ready)
        return true;

    /* De-assert reset & enable interface */
    acLinkCtrl.reset = true; /* active low; true == false :zany_face: */
    acLinkCtrl.interfaceEnable = true;
    v97_setACLinkCtrlReg(dev, acLinkCtrl);

    while (timeout--) {
        if (v97_getACLinkStatusReg(dev).ready)
            return true;
        
        sys_ioDelay(1);
    }

    return false;
}

/*  Enables / Disables AC97 SB PCM output bit 
    according to interface enablement */
static void v97_setSbPcmOutput(pci_Device dev) {
    v97_ACLinkCtrl  acLinkCtrl  = v97_getACLinkCtrlReg(dev);
    v97_FuncEnable  funcEnable  = v97_getFuncEnableReg(dev);

    acLinkCtrl.sbPcmDataOutput = funcEnable.sbEnable;
    v97_setACLinkCtrlReg(dev, acLinkCtrl);
}

/* Set up Sound Blaster interface*/
static bool v97_setupSb(pci_Device dev, bool enable, u16 port, u16 irq, u16 dma) {
    v97_FuncEnable  funcEnable  = v97_getFuncEnableReg(dev);
    v97_PnpCtrl     pnpCtrl     = v97_getPnpControlReg(dev);
    
    /*  If enabled, set appropriate bits for Port/IRQ/DMA in PnPCtrl */
    if (enable) {
        u8 sbPortBits, sbIrqBits, sbDmaBits;

        if (!v97_regBitsLookup(c_v97_sbPorts,  ARRAY_SIZE(c_v97_sbPorts), port, &sbPortBits)) return false;
        if (!v97_regBitsLookup(c_v97_sbIrqs,   ARRAY_SIZE(c_v97_sbIrqs),  irq,  &sbIrqBits))  return false;
        if (!v97_regBitsLookup(c_v97_sbDmas,   ARRAY_SIZE(c_v97_sbDmas),  dma,  &sbDmaBits))  return false;

        pnpCtrl.sbPort = sbPortBits;
        pnpCtrl.sbIrq  = sbIrqBits;
        pnpCtrl.sbDma  = sbDmaBits;

        v97_setPnpControlReg(dev, pnpCtrl);

    }

    /* Handle Function Enable */
    funcEnable.sbEnable = enable;
    v97_setFuncEnableReg(dev, funcEnable);

    /* Handle AC Link control - SB PCM output bit must be set accordingly */
    v97_setSbPcmOutput(dev);

    return enable == v97_getFuncEnableReg(dev).sbEnable;
}

/* Set up Joystick Port */
static bool v97_setupJoyPort(pci_Device dev, bool enable) {
    v97_FuncEnable  funcEnable  = v97_getFuncEnableReg(dev);

    funcEnable.joyEnable = enable;
    v97_setFuncEnableReg(dev, funcEnable);
    return enable == v97_getFuncEnableReg(dev).joyEnable;
}

/* Set up MIDI Port */
static bool v97_setupMidi(pci_Device dev, bool enable, u16 port) {
    v97_FuncEnable  funcEnable  = v97_getFuncEnableReg(dev);
    v97_PnpCtrl     pnpCtrl     = v97_getPnpControlReg(dev);

    /* Set appropriate bits for Port in PnPCtrl if enabled */
    if (enable) {
        u8 midiPortBits = 0;

        if (!v97_regBitsLookup(c_v97_midiPorts, ARRAY_SIZE(c_v97_midiPorts), port, &midiPortBits)) return false;
        
        funcEnable.midiPortCfgSwitch = 0;
        pnpCtrl.midiPort = midiPortBits;
        v97_setPnpControlReg(dev, pnpCtrl);
    }

    funcEnable.midiEnable = enable;

    v97_setFuncEnableReg(dev, funcEnable);

    return enable == v97_getFuncEnableReg(dev).midiEnable;
}

/* Set up FM (or at least as much as we can set up ourselves without the TSR) */
static bool v97_setupFM(pci_Device dev, bool enable) {
    v97_FuncEnable  funcEnable  = v97_getFuncEnableReg(dev);

    funcEnable.fmEnable = enable;
    v97_setFuncEnableReg(dev, funcEnable);

    return enable == v97_getFuncEnableReg(dev).fmEnable;
}

static void v97_printChipStatus (pci_Device dev, ac97_Interface *ac) {
    v97_PnpCtrl         pnpCtrl         = v97_getPnpControlReg(dev);
    v97_FuncEnable      funcEnable      = v97_getFuncEnableReg(dev);
    v97_ACLinkCtrl      acLinkCtrl      = v97_getACLinkCtrlReg(dev);
    v97_ACLinkStatus    acLinkStatus    = v97_getACLinkStatusReg(dev);
    u16                 sbPort, sbIrq, sbDma;
    u16                 midiPort;

    /* Parse port settings from registers */
    v97_regValueLookup(c_v97_sbPorts, sizeof(c_v97_sbPorts), pnpCtrl.sbPort, &sbPort);
    v97_regValueLookup(c_v97_sbIrqs, sizeof(c_v97_sbIrqs), pnpCtrl.sbIrq, &sbIrq);
    v97_regValueLookup(c_v97_sbDmas, sizeof(c_v97_sbDmas), pnpCtrl.sbDma, &sbDma);
    v97_regValueLookup(c_v97_midiPorts, sizeof(c_v97_midiPorts), pnpCtrl.midiPort, &midiPort);

    printf("AC'97 Codec Interface Status:\n");
    printf("    Codec Interface:            %s\n", acLinkCtrl.interfaceEnable ? "Enabled" : "Disabled");
    printf("    Interface Ready:            %s\n", acLinkStatus.ready ? "Yes" : "No");
    printf("    SoundBlaster PCM Routing:   %s\n", acLinkCtrl.sbPcmDataOutput ? "Enabled" : "Disabled");

    if (acLinkCtrl.interfaceEnable) {
        printf("    Codec ID:                   0x%08lx\n", ac97_getCodecId(ac));
        printf("    Codec Name:                 %s\n", ac97_getCodecName(ac));
    }

    printf("Legacy Audio Hardware Status:\n");

    if (funcEnable.sbEnable) {
        printf("    Sound Blaster:              Port %03x, IRQ %u, DMA %u\n", sbPort, sbIrq, sbDma);
        printf("    SB Gate on FIFO Empty:      %s\n", funcEnable.sbpcmEmptyFifoGate ? "Yes" : "No");
    } else
        printf("    Sound Blaster:              Disabled\n");

    if (funcEnable.joyEnable)
        printf("    Game Port:                  Port 201\n");
    else 
        printf("    Game Port:                  Disabled\n");

    if (funcEnable.midiEnable)
        printf("    MIDI (MPU-401):             Port %03x\n", midiPort);
    else 
        printf("    MIDI (MPU-401):             Disabled\n");

    if (funcEnable.fmEnable)
        printf("    AdLib (OPL3) Synth:         Port 388 (WARNING: Needs VIAFMTSR.EXE!)\n");
    else 
        printf("    AdLib (OPL3) Synth:         Disabled\n");

}

static void v97_printMixerStatus(ac97_Interface *ac) {
    ac97_Volume tmp;
    ac97_VolumeCtrlIdx channel;

    printf("AC'97 Mixer:\n");
    printf("    3D Surround Sound:          %s\n", ac97_getSurround(ac) ? "Enabled" : "Disabled");
    printf("\n");

    for (channel = AC97_VOL_MASTER; channel < __AC97_VOL_COUNT__; channel++) {
        ac97_getVolume(ac, channel, &tmp);

        printf("[%12s] L: %2u (%5.1f%%) R: %2u (%5.1f%%) %s\n",
            ac97_getChannelName(ac, channel),
            tmp.l, tmp.l_percent,
            tmp.r, tmp.r_percent,
            tmp.muted ? "< MUTE >" : "");
    }
}

/* Do the setup of all the volume settings requested in the command line parameters */
static bool v97_doVolumeSetup(ac97_Interface *ac) {
    u16 i;
    for (i = 0; i < (u16) __AC97_VOL_COUNT__; i++) {
        if (v97_args.volumes[i].setup) {
            u16 vol = v97_args.volumes[i].volume;
            bool mute = (vol == 0);

            if (!ac97_setVolume(ac, (ac97_VolumeCtrlIdx) i, vol, vol, mute)) {
                printf("Failed to set volume for channel '%s'\n", ac97_getChannelName(ac, (ac97_VolumeCtrlIdx) i));
                return false;
            }
        }
    }
    return true;
}

/* Adds default mixer settings to the configuration */
static void v97_addDefaultVolumesToArgs(void) {
    v97_args.volumes[AC97_VOL_MASTER].setup = true;
    v97_args.volumes[AC97_VOL_MASTER].volume = 31;

    v97_args.volumes[AC97_VOL_WAVE].setup = true;
    v97_args.volumes[AC97_VOL_WAVE].volume = 31;

    v97_args.volumes[AC97_VOL_CDIN].setup = true;
    v97_args.volumes[AC97_VOL_CDIN].volume = 31;
}

/* Perform all the setup requested in the command line parameters */
static bool v97_doSetup(pci_Device dev, bool defaultSetup) {
    pci_DeviceInfo  devInfo;
    ac97_Interface  ac;
    u16             ioBase;
    
    /* Get PCI device info so we can extract the port I/O base for the Audio Device */
    if (!pci_populateDeviceInfo(&devInfo, dev)) {
        printf("ERROR: Getting PCI Device info failed!\n");
        return false;
    }

    ioBase = (u16) devInfo.bars[0].address;

    /* Set up AC-Link so we can power up and control the codec */
    if (!v97_enableACLink(dev)) {
        printf("ERROR: Failed to enable audio codec interface (AC-Link)!\n");
        return false;
    }

    /* Do default setup if requested */
    if (defaultSetup) {
        v97_addDefaultVolumesToArgs();
        v97_setSbPcmOutput(dev);
    }

    /* Initialize the AC97 Codec and Mixer */
    if (!ac97_mixerInit(&ac, v97_codecRead, v97_codecWrite, (void*) &ioBase)) {
        printf("ERROR: AC'97 codec/mixer init failed!\n");
        return false;
    }

    /* Perform Sound blaster setup if requested */
    if (v97_args.sb.setup && !v97_setupSb(dev, v97_args.sb.enable, v97_args.sb.config[0], v97_args.sb.config[1], v97_args.sb.config[2])) {
        printf("ERROR: Failed to set up Sound Blaster\n");
        return false;
    }

    /* Perform Game Port setup if requested */
    if (v97_args.joy.setup && !v97_setupJoyPort(dev, v97_args.joy.enable)) {
        printf("ERROR: Failed to set up Game Port\n");
        return false;
    }

    /* Perform Midi Port setup if requested */
    if (v97_args.midi.setup && !v97_setupMidi(dev, v97_args.midi.enable, v97_args.midi.port)) {
        printf("ERROR: Failed to set up MIDI Port\n");
        return false;
    }

    /* Perform FM Port setup if requested */
    if (v97_args.fm.setup && !v97_setupFM(dev, v97_args.fm.enable)) {
        printf("ERROR: Failed to set up FM\n");
        return false;
    }

    /* 3d surround & mic boost */
    if (v97_args.surround.setup && !ac97_setSurround(&ac, v97_args.surround.enable)) {
        printf("ERROR: Failed to set Surround\n");
        return false;
    }
    
    if (v97_args.micBoost.setup && !ac97_setMicBoost(&ac, v97_args.micBoost.enable)) {
        printf("ERROR: Failed to set mic boost\n");
        return false;
    }

    /* Set the volumes */
    if (!v97_doVolumeSetup(&ac)) {
        printf("ERROR: Failed to set volumes. Aborting.\n");
        return false;
    }

    /* Finally, print all current settings the chip is configured with */
    printf("\n");
    v97_printChipStatus(dev, &ac);
    v97_printMixerStatus(&ac);
    printf("\n");

    return true;
}

/* Application arguments parsing */

static const char v97_versionString[] = "V97_AC97.866 Version 0.2 - (C) 2025 Eric Voirin (oerg866)";
static const char v97_appDescription[] =
    "http://github.com/oerg866/via_ac97.866\n"
    "\n"
    "VIA_AC97.866 is a driver for MS-DOS that lets you configure Legacy Audio\n"
    "features of VIA motherboard south bridges:\n"
    "        VT82C686\n"
    "        VT82C686A\n"
    "        VT82C686B\n"
    "\n"
    "Without parameters, it uses the values configured by the BIOS and only\n"
    "powers up and unmutes the audio codec.\n"
    "\n"
    "Additional configuration can be done using the following command line\n"
    "parameters.\n"
    "\n"
    "VIA_AC97.866 was built using:\n"
    "LIB866D DOS Real-Mode Software Development Library\n"
    "http://github.com/oerg866/lib866d\n";

static bool v97_argSbSetup(const void *arg) {
    UNUSED_ARG(arg);
    v97_args.sb.enable = true;
    /* Check parameters are in range */
    return v97_regBitsLookup(c_v97_sbPorts, ARRAY_SIZE(c_v97_sbPorts), v97_args.sb.config[0], NULL)
        && v97_regBitsLookup(c_v97_sbIrqs,  ARRAY_SIZE(c_v97_sbIrqs),  v97_args.sb.config[1], NULL)
        && v97_regBitsLookup(c_v97_sbDmas,  ARRAY_SIZE(c_v97_sbDmas),  v97_args.sb.config[2], NULL);
}
/* Make sure we don't try to set it up and disable it at the same time */
static bool v97_argSbDisable(const void *arg) { UNUSED_ARG(arg); return v97_args.sb.setup == false; }

static bool v97_argMidiSetup(const void *arg) {
    UNUSED_ARG(arg);
    v97_args.midi.enable = true;
    /* Check parameters are in range */
    return v97_regBitsLookup(c_v97_midiPorts, ARRAY_SIZE(c_v97_midiPorts), v97_args.midi.port, NULL);
}
/* Make sure we don't try to set it up and disable it at the same time */
static bool v97_argMidiDisable(const void *arg) { UNUSED_ARG(arg); return v97_args.midi.setup == false; }

static const args_arg v97_supportedArgs[] = {
    ARGS_HEADER(v97_versionString, v97_appDescription),
    ARGS_USAGE("?", "Prints parameter list"),

    /* Sound Blaster settings */
    { "sb",     "port,irq,dma", "Force Enable Sound Blaster", ARG_ARRAY(ARG_U16, 3), &v97_args.sb.setup, v97_args.sb.config, v97_argSbSetup},
                    ARGS_EXPLAIN("port: Sound Blaster Port"),
                    ARGS_EXPLAIN("      (0x220, 0x240, 0x260, 0x280)"),
                    ARGS_EXPLAIN("irq:  Sound Blaster IRQ"),
                    ARGS_EXPLAIN("      (5, 7, 9, 10)"),
                    ARGS_EXPLAIN("dma:  Sound Blaster DMA"),
                    ARGS_EXPLAIN("      (0, 1, 2, 3)"),
                    ARGS_EXPLAIN(""),
                    ARGS_EXPLAIN("Example: /sb:0x220,5,1"),
    { "nosb",     NULL,  "Force Disable Sound Blaster", ARG_FLAG, &v97_args.sb.setup, NULL, v97_argSbDisable }, /* Enable flag will be cleared by initial struct memset */

    /* Midi settings */
    { "midi",   "port", "Force Enable MIDI Port", ARG_ARRAY(ARG_U16, 3), &v97_args.midi.setup, &v97_args.midi.port, v97_argMidiSetup},
                    ARGS_EXPLAIN("port: Midi I/O Port"),
                    ARGS_EXPLAIN("      (0x330, 0x310, 0x320, 0x330)"),
    { "nomidi", NULL,  "Force Disable MIDI Port", ARG_FLAG, &v97_args.midi.setup, NULL, v97_argMidiDisable }, /* Enable flag will be cleared by initial struct memset */

    /* FM & Game Port */
    { "fm",     "0/1", "Disable/Enable AdLib/OPL3", ARG_BOOL, &v97_args.fm.setup, &v97_args.fm.enable, NULL },
                    ARGS_EXPLAIN("Always Hardcoded to Port 0x388!"),
                    ARGS_EXPLAIN("NOTE: This just sets a flag to enable"),
                    ARGS_EXPLAIN("      H/W Assistance for OPL3 emulation."),
                    ARGS_EXPLAIN("      It requires VIAFMTSR.EXE to function!"),

    { "joy",    "0/1", "Disable/Enable Game Port", ARG_BOOL, &v97_args.joy.setup, &v97_args.joy.enable, NULL },
                    ARGS_EXPLAIN("Always Hardcoded to Port 0x201!"),

    /*  7 Volumes - code written while fouquin rants about banks
        No checker functions, because we will sanity check them using ac97 interface structures later */
    ARGS_BLANK,

    { "v_master",   "volume",   "Mixer: Set Master Volume (0-31)",      ARG_U16, &v97_args.volumes[0].setup, &v97_args.volumes[0].volume, NULL },
    { "v_wave",     "volume",   "Mixer: Set Wave Volume (0-31)",        ARG_U16, &v97_args.volumes[1].setup, &v97_args.volumes[1].volume, NULL },
    { "v_pcspk",    "volume",   "Mixer: Set PC Speaker Volume (0-15)",  ARG_U16, &v97_args.volumes[2].setup, &v97_args.volumes[2].volume, NULL },
    { "v_mic",      "volume",   "Mixer: Set Mic Volume (0-31)",         ARG_U16, &v97_args.volumes[3].setup, &v97_args.volumes[3].volume, NULL },
    { "v_line",     "volume",   "Mixer: Set Line Input Volume (0-31)",  ARG_U16, &v97_args.volumes[4].setup, &v97_args.volumes[4].volume, NULL },
    { "v_cd",       "volume",   "Mixer: Set CD Audio Volume (0-31)",    ARG_U16, &v97_args.volumes[5].setup, &v97_args.volumes[5].volume, NULL },
    { "v_video",    "volume",   "Mixer: Set Video Input Volume (0-31)", ARG_U16, &v97_args.volumes[6].setup, &v97_args.volumes[6].volume, NULL },
    { "v_aux",      "volume",   "Mixer: Set Aux Input Volume (0-31)",   ARG_U16, &v97_args.volumes[7].setup, &v97_args.volumes[7].volume, NULL },
    { "micboost",   "0/1",      "Disable/Enable Mic Boost",             ARG_BOOL, &v97_args.micBoost.setup, &v97_args.micBoost.enable, NULL },
    { "3d",         "0/1",      "Disable/Enable 3D Surround",           ARG_BOOL, &v97_args.surround.setup, &v97_args.surround.enable, NULL },
};

int main(int argc, char *argv[]) {
    args_ParseError argErr;
    pci_Device      ac97dev;
    bool            defaultSetup = false;
    
    /* Parse command line arguments */
    memset(&v97_args, 0, sizeof(v97_args));

    argErr = args_parseAllArgs(argc, (const char **) argv, v97_supportedArgs, ARRAY_SIZE(v97_supportedArgs));

    if (argErr == ARGS_USAGE_PRINTED) {
        return 0;
    }
    
    /* Usage wasn't printed, so we need to print header */
    printf("%s\n", v97_versionString);

    if (argErr != ARGS_NO_ARGUMENTS && argErr != ARGS_SUCCESS) {
        printf("Error parsing arguments!\n");
        return 1;
    }
    
    if (argErr == ARGS_NO_ARGUMENTS) {
        printf("\nNo arguments provided - Will perform default codec configuration\n");
        printf("(Based on BIOS configuration!)\n");
        printf("Run 'via_ac97.exe /?' for more info!\n");
        defaultSetup = true;
    }


    /* Check if PCI bus is accessible on this machine */
    if (!pci_test()) {
        printf("ERROR enumerating PCI bus. Quitting...\n");
        return -1;
    }


    /* Check for VIA 686x Audio Device on the bus */
    if (!pci_findDevByID(0x1106, 0x3058, &ac97dev)) {
        printf("VIA 686 Audio Device not found!\n");
        return -1;
    }
    
    /* Perform setup based on command line arguments */
    if (!v97_doSetup(ac97dev, defaultSetup)) {
        printf("VIA_AC97 did not run successfully.\n");
        return -1;
    }


    /* Done! :D */
    printf("Success.\n");

    return 0;
}

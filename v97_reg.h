#ifndef _V97_REG_H_
#define _V97_REG_H_
#include "types.h"

#pragma pack(1)

/* Register 0x40 - AC Link Interface Status */
typedef union {
    struct {
        u8 ready                : 1;
        u8 _dont_care_          : 7;
    };
    u8 raw;
} v97_ACLinkStatus;

/* Register 0x41 - AC Link Interface Control */
typedef union {
    struct {
        u8 sbPcmDataOutput      : 1;
        u8 fmPcmDataOutput      : 1;
        u8 _dont_care_          : 4;
        u8 reset                : 1;
        u8 interfaceEnable      : 1;
    };
    u8 raw;
} v97_ACLinkCtrl;

/* Register 0x42 - Function Enable */
typedef union {
struct {   
        u8 sbEnable             : 1;
        u8 midiEnable           : 1;
        u8 fmEnable             : 1;
        u8 joyEnable            : 1;
        u8 sbpcmEmptyFifoGate   : 1;
        u8 reg2CWritable        : 1;
        u8 maskMidiIrq          : 1;
        u8 midiPortCfgSwitch    : 1;
    };
    u8 raw;
} v97_FuncEnable;

/* Register 0x43 - PnP Control */
typedef union {
    struct {
        u8 sbPort               : 2;
        u8 midiPort             : 2;
        u8 sbDma                : 2;
        u8 sbIrq                : 2;
    };
    u8 raw;
} v97_PnpCtrl;

/* Register 0x48 - FM NMI Control */
typedef union {
    struct {
        u8 fmTrapIntDisable     : 1;
        u8 fmSgdDataForSbMixing : 1;
        u8 fmIrqSelect          : 1; /* 0 = NMI, 1 = SMI */
        u8 _reserved_           : 5;
    };
    u8 raw;
} v97_FmNmiCtrl;

#define V97_PCI_REG_REVISION        (0x08)
#define V97_PCI_REG_IO_BASE_0       (0x10)
#define V97_PCI_REG_IO_BASE_1       (0x14)
#define V97_PCI_REG_IRQ_NUM         (0x3C)
#define V97_PCI_REG_AC_LINK_STATUS  (0x40)
#define V97_PCI_REG_AC_LINK_CTRL    (0x41)
#define V97_PCI_REG_FUNC_ENABLE     (0x42)
#define V97_PCI_REG_PNP_CTRL        (0x43)
#define V97_PCI_REG_FM_NMI_CTRL     (0x48)

/* Registers related to Scatter Gather DMA */
#define V97_FM_SGD_STATUS           (0x20)
#define V97_FM_SGD_CTRL             (0x21)
#define V97_FM_SGD_TYPE             (0x22)
#define V97_FM_SGD_TABLE_PTR        (0x24)
#define V97_FM_SGD_CURRENT_POS      (0x2C)

typedef union {
    struct {
        u8 flag         : 1;
        u8 EOL          : 1;
        u8 stopped      : 1;
        u8 _reserved_   : 2;
        u8 paused       : 1;
        u8 active       : 1;
    };
    u8 raw;
} v97_SgdChannelStatus;

typedef union {
    struct {
        u8 _reserved1_  : 3;
        u8 pause        : 1;
        u8 _reserved2_  : 2;
        u8 terminate    : 1;
        u8 start        : 1;
    };
    u8 raw;
} v97_SgdChannelCtrl;

typedef union {
    struct {
        u8 intOnFlag            : 1;
        u8 intOnEOL             : 1;
        u8 intSelect            : 2;    /*  00 Interrupt at PCI Read of Last Line (default)
                                            01 Interrupt at Last Sample Sent
                                            10 Interrupt at Less Than One Line to Send
                                            11 -reserved */
        u8 stereo               : 1;
        u8 is16Bit              : 1;
        u8 _reserved_           : 1;
        u8 autoStartSgdAtEOL    : 1;    /* Auto-Restart DMA when last block is played (EOL) */
    };
    u8 raw;
} v97_SgdChannelType;

typedef struct {
    u32 baseAddress;
    union {
        struct {
            u32 length   : 24;
            u32 reserved : 5;
            u32 stop     : 1;
            u32 flag     : 1;
            u32 eol      : 1;
        };
        u32 raw;
    } countFlags;
} v97_SgdTableEntry;

/* OPL3 Emulation Registers */

typedef union {
    struct {
        u8 fmNmiStatus  : 2;
        u8 _reserved_   : 6;
    };
    u8 raw;
} v97_FmNmiStatus;

#pragma pack()
#endif

/*  LIB866D mini replacement for the VFM TSR 
    The TSR must be as small as possible... 
    Sorry this is REALLY hacky... 
    Copyright notice omitted because nobody would want to copy this shit anyway :D */

#include "types.h"

#include "386asm.h"
#include "pci.h"
#include "util.h"

#include "vfm_tsr.h"

#include <conio.h>

void sys_outPortB(u16 port, u8 outVal) {
    _asm {
        mov dx, port
        mov al, outVal
        out dx, al
    }
//    printf(">>>>>>>> OUTPORTB: %04x, %02x\n", port, outVal);
}

u8 sys_inPortB(u16 port) {
    u8 retVal;
    _asm {
        mov dx, port
        in al, dx
        mov retVal, al
    }

    //    printf("<<<<<<<< INPORTB:  %04x, %02x\n", port, retVal);
    return retVal;
}

void sys_outPortL(u16 port, u32 outVal) {
    u32 _far *outValFarPtr = (u32 _far *) &outVal;
    UNUSED_ARG(outValFarPtr); /* asm macro below doesn't detect it as used */
    _asm {
        push dx
        PUSH32(_EAX)
        mov dx, port
        MOV_REG_DWORDPTR(_EAX, outValFarPtr)
        OUT_DX_EAX
        POP32(_EAX)
        pop dx
    }
//    if (port >= 0x1000)
//        printf(">>>>>>>> OUTPORTL: %04x, %08lx\n", port, outVal);
}

u32 sys_inPortL(u16 port) {
    u32 retVal = 0;
    u32 _far* retValFarPtr = &retVal;
    UNUSED_ARG(retValFarPtr); /* asm macro below doesn't detect it as used */
    _asm {
        push dx
        PUSH32(_EAX)
        mov dx, port
        IN_EAX_DX
        MOV_DWORDPTR_REG(retValFarPtr, _EAX)
        POP32(_EAX)
        pop dx
    }
//    if (port >= 0x1000)
//        printf("<<<<<<<< INPORTL:  %04x, %08lx\n", port, retVal);
    return retVal;
}

void sys_ioDelay(u16 loops) {
    while (loops--) {
        _asm {
            push ax
            mov al, 1
            out 0xED, al
            pop ax
        }
    }
}

bool pci_test(void) {
    u32 test = 0;

    /* Concept stolen from linux kernel :P */
    sys_outPortB(0xCFB, 0x01);
    test = sys_inPortL(0xCF8);
    sys_outPortL(0xCF8, 0x80000000UL);
    test = sys_inPortL(0xCF8);

    if (test != 0x80000000UL) {
        return false;
    }

    return true;
}

u32 pci_read32(pci_Device device, u32 offset)
/* Reads DWORD from PCI config space. Assumes offset is DWORD-aligned. */
{
    u32 address = ((u32)device.bus << 16UL) | ((u32)device.slot << 11UL)
        | ((u32)device.func << 8UL) | (offset & 0xFCUL)
        | 0x80000000UL;
    sys_outPortL(0xCF8, address);
    return sys_inPortL(0xCFC);
}


u16 pci_read16(pci_Device device, u32 offset)
/* Reads WORD from PCI config space. Assumes offset is WORD-aligned. */
{
    return (offset & 0x02UL)    ? (u16) (pci_read32(device, offset) >> 16UL)
                                : (u16) (pci_read32(device, offset));
}

u8 pci_read8(pci_Device device, u32 offset)
/* Reads BYTE from PCI config space. */
{
    return (u8) (pci_read32(device, offset) >> (8 * (offset % 4)));
}

void pci_write32(pci_Device device, u32 offset, u32 value)
/* Writes DWORD to PCI config space. Assumes offset is DWORD-aligned. */
{
    u32 address = ((u32)device.bus << 16UL) | ((u32)device.slot << 11UL)
        | ((u32)device.func << 8UL) | (offset & 0xFCUL)
        | 0x80000000UL;
    sys_outPortL(0xCF8, address);
    sys_outPortL(0xCFC, value);
}

void pci_write8(pci_Device device, u32 offset, u8 value) {
    u32 temp = pci_read32(device, offset);
    switch (offset & 3) {
    case 3: temp = (temp & 0x00FFFFFFUL) | ((u32) value << 24UL); break;
    case 2: temp = (temp & 0xFF00FFFFUL) | ((u32) value << 16UL); break;
    case 1: temp = (temp & 0xFFFF00FFUL) | ((u32) value <<  8UL); break;
    case 0: temp = (temp & 0xFFFFFF00UL) | ((u32) value <<  0UL); break;
    }
    pci_write32(device, offset, temp);
}


__inline u16 pci_getVendorID(pci_Device device) {
    return pci_read16(device, 0UL);
}

__inline u16 pci_getDeviceID(pci_Device device) {
    return pci_read16(device, 2UL);
}

bool pci_findDevByID(u16 ven, u16 dev, pci_Device *device) {
    for (device->bus = 0; device->bus < PCI_BUS_MAX; ++device->bus) {
        for (device->slot = 0; device->slot <= PCI_SLOT_MAX; ++device->slot) {
            for (device->func = 0; device->func <= PCI_FUNC_MAX; ++device->func) {
                if (pci_getVendorID(*device) == ven && pci_getDeviceID(*device) == dev)
                    return true;
            }
        }
    }
    return false;
}

#ifdef LOGGING
static void sys_serialInit() {
    outp(0x3fb, 0x80);
    outp(0x3F8, 0x01);
    outp(0x3fb, 0x00);
    outp(0x3F9, 0x00);
    outp(0x3fa, 0xC7);
    outp(0x3fb, 0x03);
    outp(0x3fc, 0x00);
}

void sys_serialSend(const u8 *byte, u16 length) {
    while (length) {
        u8 data = *byte;
        u16 delay;

        while (0x20 & inp(0x3fd) == 0) {}

        outp(0x3F8, data);

        /* IDK why the hell this is necessary if we check for fifo clears fucking dumb shit */
        for (delay = 0; delay < 1000UL; delay++) {
            outp(0xED, 1);
        }

        byte++;
        length--;
    }
}
#endif


static char tmpBuf[80];

void vfm_puts(const char *str) {
    u16 i = 0;

    while (1) {
        if (i >= sizeof(tmpBuf) - 2) break;
        if (*str == 0) break;
        if (*str == '\n') {
            tmpBuf[i] = '\r';
            i++;
        }
        tmpBuf[i] = *str;
        str++;
        i++;
    }
    tmpBuf[i] = '$';

    _asm {
        mov dx, offset tmpBuf
		mov ah, 0x09
		int 0x21
    }
}

bool vfm_vdsIsSupported() {
    u8 _far *vdfFlagBytePtr = MK_FP(0x40, 0x7b); /* 040:007b, bit 5 = VDS support */
    u8 errorCode = 0;
    u16 versionNumber = 0;
    u16 productNumber = 0;
    bool retVal = false;

    /*
        - Virtual DMA Specification (VDS) - GET VERSION
        DX = 0
        Return: CF clear if ok, AH=major version number, AL=minor 
        BX = product number, CX = product revision number
        SI:DI = maximum DMA buffer, DX=flags
        CF set on error, AL=error code
    */
    _asm {
        mov ax, 0x8102
        mov dx, 0
        int 0x4b
        mov errorCode, al
        jc _noVDS /* Carry set: No VDS supported */
        mov retVal, 1
        mov versionNumber, ax
        mov productNumber, bx
_noVDS:
    }

    /* QEMU identifier is */
    if (productNumber == 0x5145) {
        retVal = true;
    }

    if ((*vdfFlagBytePtr) & (1 << 5)) {
        retVal = true;
    }

    if (retVal) {
        DBG_PRINT("Virtual DMA Support detected, version %04x product ID %04x\n", versionNumber, productNumber);
    }

    return retVal;
}

bool vfm_vdsLockDmaRegion(vfm_VirtualDmaDescriptor *dds, u16 flags) {
    vfm_VirtualDmaDescriptor _far *farDds = (vfm_VirtualDmaDescriptor _far *) dds;
    bool success = true;
    u8 errorCode = 0;
    _asm {
        les di, farDds
        mov ax, 0x8103
        mov dx, flags
        int 0x4b
        mov errorCode, al
        jc _lockFail
        mov success, 1
_lockFail:
    }

    if (success) {
        DBG_PRINT("Virtual DMA Region locked, physical address %08lx\n", dds->physAddr);
    } else {
        DBG_PRINT("Virtual DMA Region lock failed, error code %02x\n", errorCode);
    }

    return success;
}

bool vfm_vdsUnlockDmaRegion(vfm_VirtualDmaDescriptor *dds) {
    vfm_VirtualDmaDescriptor _far *farDds = (vfm_VirtualDmaDescriptor _far *) dds;
    bool success = true;
    u8 errorCode = 0;
    _asm {
        les di, farDds
        mov ax, 0x8104
        xor dx, dx
        int 0x4b
        mov errorCode, al
        jc _lockFail
        mov success, 1
_lockFail:
    }

    if (success) {
        DBG_PRINT("Virtual DMA Region Unlocked\n");
    } else {
        DBG_PRINT("Virtual DMA Region Unlock failed, error code %02x\n", errorCode);
    }

    return success;
}

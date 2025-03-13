/*  LIB866D mini replacement for the VFM TSR 
    The TSR must be as small as possible... 
    Sorry this is REALLY hacky... 
    Copyright notice omitted because nobody would want to copy this shit anyway :D */

#include "types.h"

#include "386asm.h"
#include "pci.h"

#include <conio.h>

#define UNUSED_ARG(x) (void) (x)

void sys_outPortB(u16 port, u8 outVal) {
    outp(port, outVal);
//    printf(">>>>>>>> OUTPORTB: %04x, %02x\n", port, outVal);
}

u8 sys_inPortB(u16 port) {
    u8 retVal = inp(port);
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
    outp(0xCFB, 0x01);
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

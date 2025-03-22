# VIA_AC97.866 - VIA 686 AC'97 DOS Sound Driver & OPL3 Emulation TSR

(C) 2025 Eric Voirin (oerg866)

---
DBOPL (C) 2002-2021 The DOSBox Team
Nuked-OPL3 (C) 2013-2020 Nuke.YKT
---

![image](assets/screen1.png)

# Description

**VIA_AC97.866** is a driver and mixer configuration program for MS-DOS that lets you configure Legacy Audio features of VIA motherboard south bridges, found on many old VIA-based motherboards in the late 1990s and early 2000s.

These chipsets have hardware-based legacy audio support, with lackluster and barely functional vendor-provided drivers.

**VIA_AC97.866** was built using the [LIB866D DOS Real-Mode Software Development Library](https://github.com/oerg866/lib866d)

It consists of two parts:

## `VIA_AC97.EXE`, the setup and mixer utility

This utility lets you enable/disable the legacy audio features of your VIA chipset, and lets you set the volumes of all audio inputs and outputs.

**Note:** Compared to the official VIA drivers, it does *not* need a TSR to enable basic Sound Blaster, MIDI and Game Port functionality.

In addition, this program lets you change the volume mixer settings, which the VIA drivers do not support.

Without parameters, it uses the values configured by the BIOS and only powers up and unmutes the audio codec.

Additional configuration can be done using command line parameters.

## `V97TSR.EXE`, the OPL3 Emulation Driver

This TSR program provides the software component for the hardware-assisted FM emulation. It is intended to replace `VIAFMTSR.EXE`.

Since port trapping and NMI assertion is done 100% in hardware, this does *not* interfere with protected or real-mode DOS software.

# Features:

## `VIA_AC97.EXE` - Audio Setup & Mixer Utility
- [x] Set up AC97 audio codec
- [x] Configure Sound Blaster Legacy Audio
    - Enable/Disable
    - Port
    - IRQ
    - DMA
- [x] Configure MIDI (MPU-401) Port
    - Enable/Disable
    - Port
- [x] Configure Game Port
- [x] Enable/Disable VIA OPL3 TSR support
- [x] Override BIOS configuration
    - Can help with poor BIOS implementations
- [x] Mixer volume settings
    - Master Volume
    - Wave Out
    - PC Speaker
    - Microphone
    - Line In
    - CD Audio
    - Video In
    - Auxiliary
    - Secondary Line Output
- [x] Advanced Codec Settings
    - Variable Sample Rate (experimental)
- [x] Enable/Disable "3D Surround Sound"
- [x] Enable/Disable Microphone 20dB Boost

## `V97TSR.EXE` - OPL3 FM Emulation TSR
- [x] VIAFMTSR.EXE replacement
- [x] Much higher accuracy using DosBOX OPL3 emulation core
- [x] Works with Real Mode Software
- [x] Works with Protected Mode Software
- [ ] Unloadable (TODO)
- [ ] UMB capable (It may be, I dunno, need to figure out how to do explicitly otherwise)
- [ ] Optimized for memory footprint (TODO)

# System Requirements

* 300MHz CPU or higher 500-600MHz recommended for headroom in heavy games/applications
* Supported VIA chipset 
* MS-DOS 5.0 or higher, or compatible

## Supported VIA Chipset Southbridges

* VT82C686
* VT82C686A
* VT82C686B
* VT8231 (VIA EPIA)

These are found on many old Pentium 1/2/3 and Athlon/Duron motherboards, so you may be in luck!

# Running the Setup Utility `VIA_AC97.EXE`

`VIA_AC97.EXE <parameters>`

## Command Line

VIA_AC97.EXE supports the following command line parameters:

| Argument | Description |
| -------- | ------- |
| `/sb:<port>,<irq>,<dma>`  | Enable Sound Blaster |
|                           | `port`: I/O Port (0x220, 0x240, 0x260, 0x280) |
|                           | `irq`: IRQ (5, 7, 9, 10) |
|                           | `dma`: DMA channel (0, 1, 2 ,3) |
|                           | Example: `/sb:0x220,5,1` |
| `/nosb`                   | Force Disable Sound Blaster |
| `/midi:<port>`            | Enable MIDI Port |
|                           | `port`: I/O Port (0x300, 0x310, 0x320, 0x330) |
| `/nomidi`                 | Force Disable MIDI Port |
| `/fm:<0/1>`               | Disable/Enable Adlib/OPL3 (Always hardcoded to port 0x388!) |
|                           | *NOTE: This just sets a flag to enable  H/W Assistance for OPL3 emulation. It requires `V97TSR.EXE` to function!* |
| `/joy:<0/1>`              | Disable/Enable Game Port (Always hardcoded to port 0x201!) |
| `/v_master:<volume>`      | Mixer: Set Master Volume (0-31) |
| `/v_wave:<volume>`        | Mixer: Set Wave Volume (0-31) |
| `/v_pcspk:<volume>`       | Mixer: Set PC Speaker Volume (0-15) |
| `/v_mic:<volume>`         | Mixer: Set Mic Volume (0-31) |
| `/v_cd:<volume>`          | Mixer: Set CD Audio Volume (0-31) |
| `/v_video:<volume>`       | Mixer: Set Video Volume (0-31) |
| `/v_aux:<volume>`         | Mixer: Set Auxiliary Volume (0-31) |
| `/v_line2:<volume>`       | Mixer: Set Line Out 2 Volume (0-31) |
| `/micbooost:<0/1>`        | Disable/Enable 20dB Mic Boost |
| `/3d:<0/1>`               | Disable/Enable '3D Surround Sound' |
| `/rate:<hz>`              | Force AC97 Codec Sample Rate (experimental). |
|                           | `hz:` Sample rate in Hertz. |
|                           | A value of 0 forcefully disables the variable sample rate in the codec |

# Running the FM Emulation Driver `V97TSR.EXE`

`V97TSR.EXE <parameter>`

| Argument | Description |
| -------- | ------- |
| `r`  | Load Driver |
| `g`  | **DEBUG**: Initialize, play a test tone and wait for key press. Does *not* load the driver resident. |
| `p`  | **DEBUG**: Send a test tone on the OPL ports. Does not initialize hardware, works even with other OPLs. Does *not* load the driver resident. |


# Building Guide

## Prerequisites
* Any 32/64 Bit Windows (Windows 9x, NT, 2000, XP, Vista, 7, 8, 10, 11, etc.)
    * Yes, everything in this tool chain contains 32-Bit executables that still work fine on modern Windows
    * Linux with WINE also works
    * Building on DOS also works with a DOS extender (DOSXNT, HXDOS, etc.) and DPMI host

* **Microsoft C/C++ Optimizing Compiler Version 8.00c**
    * Also known as **Microsoft Visual C++ 1.52c**
    * Can also be built with Microsoft C/C++ version 7.00, but due to missing 386 support it is not recommended for performance reasons
* **Microsoft Macro Assembler 6.11 or higher**
* **Microsoft Segmented Executable Linker 5.60** (included with C 8.00)

## Building

* Make sure your build environment is set up correctly:

    ```
    SET BASE=C:\MSC800
    SET PATH=%BASE%\BIN;C:\MASM611\BIN;
    Set LIB=%BASE%\LIB;%BASE%\MFC\LIB;C:\MASM611\LIB;
    Set INCLUDE=%BASE%\INCLUDE;%BASE%\MFC\INCLUDE;C:\MASM611\INCLUDE
    Set HELPFILES=C:\MASM611\HELP\*.HLP;%BASE%\HELP\*.HLP
    Set INIT=%BASE%\INIT
    SET ASMEX=C:\MASM611\SAMPLES
    SET TMP=C:\TEMP
    ```

* Run `nmake` in the repository folder
* That's it!

### Extra Utility build options
* None (yet)

### Extra TSR build options
* `DEBUG=1` enables debug printouts (at the cost of bigger executable size)
* `NUKED=1` enables Nuked-OPL3 core (experimental and very slow compared to the default)
* `DBG_BUFFER=1` saves the DMA buffers to `dump.bin` when exiting doing test tone generation
* `DBG_FILE=1` enables `f` parameter which plays a 16 Bit 24KHz stereo raw PCM file `.\test.snd` on the FM DMA channel


# License

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc/4.0/deed.en)

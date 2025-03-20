CC  = cl
ASM = ml
LINK = link

CFLAGS = /c /f- /W3 /G3 /FPi87 /Gx /Os /Gs /DDOS16 /ILIB866D /I. /nologo
CFLAGS_OPL = /c /f- /W3 /G3 /FPi87 /Gx /Ox /Gs /DDOS16 /ILIB866D /I. /nologo
AFLAGS = /c /Cx /W2 /WX /nologo

!IF "$(DEBUG)"=="1"
CFLAGS = $(CFLAGS) /DDEBUG
!ENDIF

!IF "$(DBG_BENCH)"=="1"
CFLAGS = $(CFLAGS) /DDBG_BENCH
!ENDIF


!IF "$(NUKED)"=="1"
# Nuked OPL
!MESSAGE Using NUKED-OPL3
CFLAGS_TSR = $(CFLAGS) /DNUKED
AFLAGS = $(AFLAGS) /DNUKED
OPL_C = nukedopl/opl3.c
ISR_ASM = vfm_inuk.asm
!ELSE
# DOSBOX OPL
!MESSAGE Using DOSBox OPL3 Core
CFLAGS_TSR = $(CFLAGS) /DDBOPL /DPRECALC_TBL
CFLAGS_OPL = $(CFLAGS_OPL) /DPRECALC_TBL
AFLAGS = $(AFLAGS) /DDBOPL
OPL_C = dbopl/dbopl.c
ISR_ASM = vfm_idb.asm
!ENDIF

!IF "$(SAMPS_PER_BUF)"==""
SAMPS_PER_BUF = 128
!ENDIF

!IF "$(NUM_BUFS)"==""
NUM_BUFS = 3
!ENDIF

CFLAGS_DMA = $(CFLAGS_TSR) /DSAMPS_PER_BUF=$(SAMPS_PER_BUF) /DNUM_BUFS=$(NUM_BUFS)
AFLAGS_DMA = $(AFLAGS) /DSAMPS_PER_BUF=$(SAMPS_PER_BUF) /DNUM_BUFS=$(NUM_BUFS)

TARGETS : clean VIA_AC97.EXE V97TSR.EXE

# !IF "$(LOGGING)"=="1"
# !MESSAGE Building with LOGGING ENABLED
# CFLAGS = $(CFLAGS) /DLOGGING
# !ENDIF

OBJ_LIB866D = lib866d\pci.obj lib866d\vgacon.obj lib866d\sys.obj lib866d\util.obj lib866d\args.obj lib866d\ac97.obj

clean:
    del v97_*.obj
    del lib866d\*.obj
    del vfm_*.obj

VIA_AC97.EXE : clean $(OBJ_LIB866D) v97_main.obj
    $(LINK) $(OBJ_LIB866D) v97_main.obj,via_ac97.exe,,,,,

V97TSR.EXE : clean
    $(ASM) $(AFLAGS) vfm_opt.asm
    $(ASM) $(AFLAGS_DMA) /Fovfm_isr.obj $(ISR_ASM)
    $(CC) $(CFLAGS_TSR) vfm_main.c
	$(CC) $(CFLAGS_TSR) vfm_mini.c
	$(CC) $(CFLAGS_DMA) vfm_tsr.c

    $(CC) $(CFLAGS_OPL) /Fovfm_opl3.obj $(OPL_C)

    $(LINK)  vfm_opt.obj vfm_opl3.obj vfm_mini.obj vfm_isr.obj vfm_main.obj vfm_tsr.obj,v97tsr.exe,,,,,

.c: lib866d
.c.obj :
        $(CC) $(CFLAGS) /Fo$@ $< 
CC  = cl
ASM = ml
LINK = link

# Default values for buffer count and samples per buffer , can be overridden by commandline
!IF "$(SAMPS_PER_BUF)"==""
SAMPS_PER_BUF = 128
!ENDIF

!IF "$(NUM_BUFS)"==""
NUM_BUFS = 3
!ENDIF

# Flags for compilation, we compile TSR with Size optimization except OPL core for SPEEEEED~
CFLAGS_TSR = /c /f- /W3 /G3 /Gx /Os /Gs /DDOS16 /DSAMPS_PER_BUF=$(SAMPS_PER_BUF) /DNUM_BUFS=$(NUM_BUFS) /ILIB866D /I. /nologo
CFLAGS_OPL = /c /f- /W3 /G3 /Gx /Ox /Gs /DDOS16  /ILIB866D /I. /nologo
AFLAGS = /c /Cx /W2 /WX /DSAMPS_PER_BUF=$(SAMPS_PER_BUF) /DNUM_BUFS=$(NUM_BUFS) /nologo
LFLAGS = /NODEFAULTLIB

# Extra overrides for debug and benchmark (DBG_BENCH makes no sense without DEBUG...)
!IF "$(DBG_BENCH)"=="1"
DEBUG = 1
CFLAGS_TSR = $(CFLAGS_TSR) /DDBG_BENCH
!ENDIF

# In debug mode, we don't link with /NODEFAULTLIB because we need printf...
!IF "$(DEBUG)"=="1"
CFLAGS_TSR = $(CFLAGS_TSR) /DDEBUG
AFLAGS = $(AFLAGS) /DDEBUG
LFLAGS = 
!ENDIF

# OPL Emulation core selection (default is DBOPL, nmake NUKED=1 would override this)
!IF "$(NUKED)"=="1"
# Nuked OPL
!MESSAGE Using NUKED-OPL3
CFLAGS_TSR = $(CFLAGS_TSR) /DNUKED
AFLAGS = $(AFLAGS) /DNUKED
OPL_C = nukedopl/opl3.c
OBJ_ISR = vfm_inuk.obj vfm_opt.obj
!ELSE
# DOSBOX OPL
!MESSAGE Using DOSBox OPL3 Core
CFLAGS_TSR = $(CFLAGS_TSR) /DDBOPL /DPRECALC_TBL
CFLAGS_OPL = $(CFLAGS_OPL) /DPRECALC_TBL
AFLAGS = $(AFLAGS) /DDBOPL
OPL_C = dbopl/dbopl.c
OBJ_ISR = vfm_idb.obj
!ENDIF

TARGETS : clean VIA_AC97.EXE V97TSR.EXE

# Object files for LIB866D
OBJ_LIB866D = lib866d\pci.obj lib866d\vgacon.obj lib866d\sys.obj lib866d\util.obj lib866d\args.obj lib866d\ac97.obj
# C object files for TSR - Note, OPL3 core is missing from this list because it is compiled with different flags
OBJ_TSR_C = vfm_main.obj vfm_mini.obj vfm_tsr.obj
# ASM object files for TSR
OBJ_TSR_ASM = vfm_clib.obj $(OBJ_ISR)


clean:
    del v97_*.obj
    del lib866d\*.obj
    del vfm_*.obj

# Build target
VIA_AC97.EXE : clean $(OBJ_LIB866D) v97_main.obj
    $(LINK) $(OBJ_LIB866D) v97_main.obj,via_ac97.exe,,,,,
    dir via_ac97.exe

# Build target: TSR
V97TSR.EXE : clean $(OBJ_TSR_C) $(OBJ_TSR_ASM)
    $(CC) $(CFLAGS_OPL) /Fovfm_opl3.obj $(OPL_C)

    $(LINK) $(LFLAGS) $(OBJ_TSR_ASM) vfm_opl3.obj $(OBJ_TSR_C),v97tsr.exe,,,,,
    dir v97tsr.exe

# .C files in lib866d subdir
.c: lib866d

# Automatic C->OBJ
.c.obj :
        $(CC) $(CFLAGS_TSR) /Fo$@ $< 

# Automatic ASM->OBJ
.asm.obj :
        $(ASM) $(AFLAGS) /Fo$@ $< 
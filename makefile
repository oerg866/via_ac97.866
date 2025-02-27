CC = wcc
AS = wasm
LD = wlink
CL = wcl

CFLAGS = -3 -bt=dos -i=lib866d -wx
LDFLAGS = SYSTEM DOS

OBJ = lib866d\pci.obj lib866d\vgacon.obj lib866d\sys.obj lib866d\util.obj lib866d\args.obj lib866d\ac97.obj v97_main.obj

all : VIA_AC97.EXE

VIA_AC97.EXE : $(OBJ)
    $(LD) $(LDFLAGS) NAME VIA_AC97 FILE { lib866d\*.obj *.obj }

.c: lib866d
.c.obj : .AUTODEPEND
        $(CC) $(CFLAGS) -fo=$@ $<

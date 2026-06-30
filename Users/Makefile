CC      := gcc
CFLAGS  := -m64 -ffreestanding -fno-stack-protector -mno-red-zone -nostdlib \
           -static -fno-pie -ISource/Include -O2
LDFLAGS := -nostdlib -static -T Users/user.ld

OUTDIR := Bin/Users

PROGS := $(OUTDIR)/hello.elf $(OUTDIR)/getpid.elf

.PHONY: all user-clean

all: $(OUTDIR)/crt0.o $(OUTDIR)/libtos.a $(PROGS)

$(OUTDIR):
	mkdir -p $@

$(OUTDIR)/crt0.o: Users/crt0.S | $(OUTDIR)
	$(CC) -c -m64 $< -o $@

$(OUTDIR)/libtos.a: $(OUTDIR)/libtos.o
	ar rcs $@ $<

$(OUTDIR)/libtos.o: Users/libtos.c Users/libtos.h | $(OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Исправлено: добавлены пути $(OUTDIR)/
$(OUTDIR)/hello.elf: Users/hello.c $(OUTDIR)/crt0.o $(OUTDIR)/libtos.a | $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(OUTDIR)/getpid.elf: Users/getpid.c $(OUTDIR)/crt0.o $(OUTDIR)/libtos.a | $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

user-clean:
	rm -f $(PROGS) $(OUTDIR)/crt0.o $(OUTDIR)/libtos.o $(OUTDIR)/libtos.a
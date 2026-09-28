# QRT - build the kernel as UEFI applications for both firmware flavours and
# pack them into one bootable GPT disk image.
#
#   make            -> build/qrt.img  (write this to a USB stick / microSD)
#   make run        -> boot it in QEMU with 32-bit UEFI (like the Venue 8 Pro 5830)
#   make run64      -> boot it in QEMU with 64-bit UEFI (like the Venue 8 Pro 5855)
#   make test       -> headless boot + scripted touch test + screenshots

ifeq ($(origin CC),default)
CC := clang
endif
LINK    := lld-link
PYTHON  ?= python3
HOSTCC  ?= gcc

SRC := src/kernel/kernel.c src/kernel/rt.c src/kernel/hal.c src/kernel/sysinfo.c src/kernel/hwreport.c src/kernel/smp.c \
       src/kernel/acpi.c src/kernel/vfs.c src/kernel/dev.c src/drivers/pci.c src/drivers/uart.c src/drivers/builtin.c \
       src/ui/gfx.c src/ui/shell.c src/ui/fontdata.c \
       src/apps/clock.c src/apps/sketch.c src/apps/files.c src/apps/system.c \
       src/apps/settings.c src/apps/life.c src/apps/lab.c src/apps/terminal.c \
       src/drivers/dwi2c.c src/drivers/i2chid.c src/drivers/hidparse.c src/drivers/touch.c

CFLAGS := -std=c11 -O2 -ffreestanding -fno-stack-protector -fno-stack-check \
          -fshort-wchar -fno-math-errno -fno-asynchronous-unwind-tables \
          -mno-stack-arg-probe -Wall -Wextra -Wno-unused-parameter \
          -Wno-missing-field-initializers -Wno-sign-compare

IA32_CFLAGS := -target i686-unknown-windows-gnu -march=i686 -mno-sse -mno-mmx
X64_CFLAGS  := -target x86_64-unknown-windows-gnu -mno-red-zone

LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib

# the native kernel (ExitBootServices, own MM/interrupts/SMP) is 64-bit only
X64_SRC := src/arch/x64/mm.c src/arch/x64/cpu.c src/arch/x64/apic.c src/arch/x64/native.c \
           src/arch/x64/sched.c src/arch/x64/smp_native.c src/arch/x64/proc.c src/arch/x64/linux.c src/arch/x64/irq.c
X64_ASM := src/arch/x64/isr.S src/arch/x64/entry.S src/arch/x64/trampoline.S

IA32_OBJ := $(SRC:src/%.c=build/ia32/%.o)
X64_OBJ  := $(SRC:src/%.c=build/x64/%.o) $(X64_SRC:src/%.c=build/x64/%.o) $(X64_ASM:src/%.S=build/x64/%.o)

OVMF_DIR  ?= /usr/share/OVMF
OVMF32    := $(OVMF_DIR)/OVMF32_CODE_4M.fd
OVMF32VAR := $(OVMF_DIR)/OVMF32_VARS_4M.fd
OVMF64    := $(OVMF_DIR)/OVMF_CODE_4M.fd
OVMF64VAR := $(OVMF_DIR)/OVMF_VARS_4M.fd

.PHONY: all efi run run64 test check clean fonts

all: build/qrt.img

efi: build/BOOTIA32.EFI build/BOOTX64.EFI

src/ui/fontdata.c: tools/mkfont.py
	$(PYTHON) tools/mkfont.py assets $@

build/ia32/%.o: src/%.c $(wildcard src/*.h src/*/*.h src/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(IA32_CFLAGS) -c $< -o $@

build/x64/%.o: src/%.c $(wildcard src/*.h src/*/*.h src/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(X64_CFLAGS) -c $< -o $@

build/x64/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-windows-gnu -c $< -o $@

build/BOOTIA32.EFI: $(IA32_OBJ)
	$(LINK) $(LDFLAGS) -machine:x86 -out:$@ $^

build/BOOTX64.EFI: $(X64_OBJ)
	$(LINK) $(LDFLAGS) -machine:x64 -map:build/BOOTX64.map -out:$@ $^

# Linux programs shipped in /bin (run by the native kernel's Linux layer)
BUSYBOX ?= /bin/busybox
ROOTFS := build/rootfs/bin/hello build/rootfs/bin/hello-musl build/rootfs/bin/busybox

build/rootfs/bin/hello: tests/linux/hello.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -static-pie -O2 -s -o $@ $<

build/rootfs/bin/hello-musl: tests/linux/hello.c
	@mkdir -p $(dir $@)
	musl-gcc -static -O2 -s -o $@ $< || cp build/rootfs/bin/hello $@

build/rootfs/bin/busybox:
	@mkdir -p $(dir $@)
	@if [ -x $(BUSYBOX) ] && file -L $(BUSYBOX) | grep -q 'statically linked'; then \
	    cp $(BUSYBOX) $@; cp tools/BUSYBOX.txt build/rootfs/bin/BUSYBOX.txt; \
	else echo "no static busybox on this host (apt install busybox-static) - skipping"; touch $@; fi

build/qrt.img: build/BOOTIA32.EFI build/BOOTX64.EFI tools/mkimage.sh $(wildcard image/*) $(ROOTFS)
	tools/mkimage.sh $@ build/BOOTIA32.EFI build/BOOTX64.EFI build/rootfs

run: build/qrt.img
	tools/run-qemu.sh ia32

run64: build/qrt.img
	tools/run-qemu.sh x64

test: build/qrt.img
	$(PYTHON) tools/qemu-test.py ia32
	$(PYTHON) tools/qemu-test.py x64

check:
	@mkdir -p build
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_hid tests/test_hid.c && build/test_hid

clean:
	rm -rf build

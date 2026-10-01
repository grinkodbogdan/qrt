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
       src/kernel/acpi.c src/kernel/vfs.c src/kernel/dev.c src/drivers/pci.c src/drivers/uart.c src/drivers/builtin.c src/drivers/buttons.c src/drivers/backlight.c src/drivers/audio.c src/drivers/e1000.c \
       src/drivers/iwm/iwm.c src/drivers/i915/gpu.c src/drivers/usb/xhci.c src/drivers/bt/hci.c src/drivers/bt/btusb.c src/net/wifilog.c src/net/crypto.c src/net/crypto_tls.c src/net/tls.c src/net/http.c src/net/net.c src/net/tcp.c src/net/wlan.c src/net/netstack.c \
       src/ui/gfx.c src/ui/shell.c src/ui/fontdata.c \
       src/apps/clock.c src/apps/sketch.c src/apps/files.c src/apps/system.c \
       src/apps/settings.c src/apps/life.c src/apps/lab.c src/apps/terminal.c src/apps/wifi.c src/apps/browser.c src/apps/bluetooth.c src/apps/html.c src/ui/osk.c \
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
           src/arch/x64/sched.c src/arch/x64/smp_native.c src/arch/x64/proc.c src/arch/x64/linux.c src/arch/x64/irq.c src/arch/x64/lsock.c
X64_ASM := src/arch/x64/isr.S src/arch/x64/entry.S src/arch/x64/trampoline.S

IA32_OBJ := $(SRC:src/%.c=build/ia32/%.o)
X64_OBJ  := $(SRC:src/%.c=build/x64/%.o) $(X64_SRC:src/%.c=build/x64/%.o) $(X64_ASM:src/%.S=build/x64/%.o)

OVMF_DIR  ?= /usr/share/OVMF
OVMF32    := $(OVMF_DIR)/OVMF32_CODE_4M.fd
OVMF32VAR := $(OVMF_DIR)/OVMF32_VARS_4M.fd
OVMF64    := $(OVMF_DIR)/OVMF_CODE_4M.fd
OVMF64VAR := $(OVMF_DIR)/OVMF_VARS_4M.fd

.PHONY: check-tls all efi run run64 test check clean fonts

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
ROOTFS := build/rootfs/bin/hello build/rootfs/bin/hello-musl build/rootfs/bin/busybox \
          build/rootfs/bin/dynhello build/rootfs/bin/threads build/rootfs/bin/cxx build/rootfs/bin/procs build/rootfs/lib64/ld-linux-x86-64.so.2

# Dynamically linked programs and the host's glibc / libstdc++ they run with
# (ld.so in /lib64, the libraries in /lib/x86_64-linux-gnu, as on Debian/Ubuntu)
GLIBC_LIBS := libc.so.6 libm.so.6 libstdc++.so.6 libgcc_s.so.1
build/rootfs/lib64/ld-linux-x86-64.so.2: tools/LIBS.txt
	@mkdir -p build/rootfs/lib64 build/rootfs/lib/x86_64-linux-gnu
	cp -L /lib64/ld-linux-x86-64.so.2 $@
	for l in $(GLIBC_LIBS); do cp -L /lib/x86_64-linux-gnu/$$l build/rootfs/lib/x86_64-linux-gnu/; done
	cp tools/LIBS.txt build/rootfs/lib/x86_64-linux-gnu/README.txt

build/rootfs/bin/dynhello: tests/linux/dynhello.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -s -o $@ $<

build/rootfs/bin/threads: tests/linux/threads.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -s -o $@ $< -lpthread

build/rootfs/bin/procs: tests/linux/procs.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -s -o $@ $<

build/rootfs/bin/cxx: tests/linux/cxx.cpp
	@mkdir -p $(dir $@)
	g++ -O2 -s -o $@ $<

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

build/qrt.img: build/BOOTIA32.EFI build/BOOTX64.EFI tools/mkimage.sh $(wildcard image/*) $(wildcard firmware/*) $(ROOTFS)
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
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_crypto tests/test_crypto.c && build/test_crypto
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -O1 -fshort-wchar -o build/test_wlan tests/test_wlan.c && build/test_wlan
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_http tests/test_http.c && build/test_http
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_html tests/test_html.c && build/test_html
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O1 -fshort-wchar -o build/test_gpu tests/test_gpu.c -lm && build/test_gpu
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O1 -fshort-wchar -o build/test_bt tests/test_bt.c && build/test_bt

# TLS 1.3 client against a local OpenSSL server (needs openssl and python3)
check-tls:
	@mkdir -p build
	$(HOSTCC) -Wall -Wextra -Wno-unused-function -O2 -o build/test_tls tests/test_tls.c && tools/tls-test.sh

clean:
	rm -rf build

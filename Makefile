# QRT - build the kernel as a 64-bit UEFI application and pack it into a bootable GPT
# disk image (since 0.9.0 x86-64 only: the Venue 8 Pro 5855; the 32-bit build for the
# 5830 can still be made with `make build/BOOTIA32.EFI`, but it is not tested or shipped).
#
#   make            -> build/qrt.img  (write this to a USB stick / microSD)
#   make run        -> boot it in QEMU with 64-bit UEFI (like the Venue 8 Pro 5855)
#   make test       -> headless boot + scripted touch test + screenshots

ifeq ($(origin CC),default)
CC := clang
endif
LINK    := lld-link
PYTHON  ?= python3
HOSTCC  ?= gcc

SRC := src/kernel/kernel.c src/kernel/time.c src/kernel/sound.c src/kernel/rt.c src/kernel/hal.c src/kernel/sysinfo.c src/kernel/hwreport.c src/kernel/smp.c \
       src/kernel/acpi.c src/kernel/power.c src/kernel/install.c src/kernel/vfs.c src/kernel/dev.c src/drivers/pci.c src/drivers/uart.c src/drivers/builtin.c src/drivers/buttons.c src/drivers/backlight.c src/drivers/audio.c src/drivers/speaker.c src/drivers/hda.c src/drivers/ish.c src/drivers/battery.c src/drivers/pmic.c src/drivers/e1000.c \
       src/drivers/iwm/iwm.c src/drivers/iwn/iwn.c src/drivers/wifi.c src/drivers/i915/gpu.c src/drivers/i915/ivb.c src/drivers/i915/display.c src/drivers/usb/xhci.c src/drivers/usb/ehci.c src/drivers/usb/uaudio.c src/drivers/bt/hci.c src/drivers/bt/l2cap.c src/drivers/bt/a2dp.c src/drivers/bt/sbc.c src/drivers/bt/btusb.c src/net/wifilog.c src/net/crypto.c src/net/crypto_tls.c src/net/tls.c src/net/http.c src/net/net.c src/net/tcp.c src/net/wlan.c src/net/netstack.c \
       src/ui/gfx.c src/ui/shell.c src/ui/clientwin.c src/ui/fontdata.c \
       src/apps/clock.c src/apps/sketch.c src/apps/files.c src/apps/system.c \
       src/apps/settings.c src/apps/life.c src/apps/lab.c src/apps/terminal.c src/apps/wifi.c src/apps/browser.c src/apps/bluetooth.c src/apps/html.c src/ui/osk.c \
       src/drivers/dwi2c.c src/drivers/i2chid.c src/drivers/hidparse.c src/drivers/hidmouse.c src/drivers/touch.c

CFLAGS := -std=c11 -O2 -ffreestanding -fno-stack-protector -fno-stack-check \
          -fshort-wchar -fno-math-errno -fno-asynchronous-unwind-tables \
          -mno-stack-arg-probe -Wall -Wextra -Wno-unused-parameter \
          -Wno-missing-field-initializers -Wno-sign-compare

IA32_CFLAGS := -target i686-unknown-windows-gnu -march=i686 -mno-sse -mno-mmx
X64_CFLAGS  := -target x86_64-unknown-windows-gnu -mno-red-zone

LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib

# the native kernel (ExitBootServices, own MM/interrupts/SMP) is 64-bit only
X64_SRC := src/arch/x64/mm.c src/arch/x64/cpu.c src/arch/x64/apic.c src/arch/x64/native.c \
           src/arch/x64/sched.c src/arch/x64/smp_native.c src/arch/x64/proc.c src/arch/x64/linux.c src/arch/x64/irq.c src/arch/x64/lsock.c src/arch/x64/signal.c src/arch/x64/lfile.c src/arch/x64/unix.c src/arch/x64/qrtcall.c src/arch/x64/lkldev.c
X64_ASM := src/arch/x64/isr.S src/arch/x64/entry.S src/arch/x64/trampoline.S src/drivers/iwn/fw.S

IA32_OBJ := $(SRC:src/%.c=build/ia32/%.o)
# ACPICA (Intel's ACPI interpreter, as in Linux) and QRT's ACPI drivers on top of it
ACPICA_SRC := $(wildcard src/acpi/acpica/*/*.c)
ACPI_SRC   := src/acpi/osl.c src/acpi/acpidev.c
ACPI_CFLAGS := -D__QRT__ -Isrc/acpi/acpica/include
X64_OBJ  := $(SRC:src/%.c=build/x64/%.o) $(X64_SRC:src/%.c=build/x64/%.o) $(X64_ASM:src/%.S=build/x64/%.o) \
            $(ACPICA_SRC:src/%.c=build/x64/%.o) $(ACPI_SRC:src/%.c=build/x64/%.o)

OVMF_DIR  ?= /usr/share/OVMF
OVMF32    := $(OVMF_DIR)/OVMF32_CODE_4M.fd
OVMF32VAR := $(OVMF_DIR)/OVMF32_VARS_4M.fd
OVMF64    := $(OVMF_DIR)/OVMF_CODE_4M.fd
OVMF64VAR := $(OVMF_DIR)/OVMF_VARS_4M.fd

.PHONY: check-tls all efi run run64 test check clean fonts arm64 mi-a1 dts-tissot

all: build/qrt.img

efi: build/BOOTX64.EFI

src/ui/fontdata.c: tools/mkfont.py
	$(PYTHON) tools/mkfont.py assets $@

build/ia32/%.o: src/%.c $(wildcard src/*.h src/*/*.h src/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(IA32_CFLAGS) -c $< -o $@

# the clock's floor: a date before this commit means the RTC was reset (time.c)
build/x64/kernel/time.o: CFLAGS += -DQRT_BUILD_EPOCH=$(shell git log -1 --format=%ct 2>/dev/null || echo 1767225600)ull

build/x64/acpi/acpica/%.o: src/acpi/acpica/%.c $(wildcard src/acpi/acpica/include/*.h src/acpi/acpica/include/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(X64_CFLAGS) $(ACPI_CFLAGS) -w -c $< -o $@

build/x64/acpi/%.o: src/acpi/%.c $(wildcard src/acpi/*.h src/acpi/acpica/include/*.h src/acpi/acpica/include/*/*.h src/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(X64_CFLAGS) $(ACPI_CFLAGS) -c $< -o $@

build/x64/%.o: src/%.c $(wildcard src/*.h src/*/*.h src/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(X64_CFLAGS) -c $< -o $@

build/x64/drivers/iwn/fw.o: firmware/iwlwifi-6000g2b-6.ucode
build/x64/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-windows-gnu -c $< -o $@

build/BOOTIA32.EFI: $(IA32_OBJ)
	$(LINK) $(LDFLAGS) -machine:x86 -out:$@ $^

# two links: the first one's map names the functions (tools/symtab.py) for the crash screen;
# the table is data placed after the code, so the code does not move in the second
build/BOOTX64.EFI: $(X64_OBJ) tools/symtab.py
	@mkdir -p build/x64/sym
	echo 'const unsigned qrt_nsyms = 0; const unsigned qrt_sym_off[1], qrt_sym_name[1]; const char qrt_sym_str[1];' > build/x64/sym/none.c
	$(CC) $(CFLAGS) $(X64_CFLAGS) -c build/x64/sym/none.c -o build/x64/sym/none.o
	$(LINK) $(LDFLAGS) -machine:x64 -map:build/x64/sym/pass1.map -out:build/x64/sym/pass1.efi $(X64_OBJ) build/x64/sym/none.o
	$(PYTHON) tools/symtab.py build/x64/sym/pass1.map build/x64/sym/symtab.c
	$(CC) $(CFLAGS) $(X64_CFLAGS) -c build/x64/sym/symtab.c -o build/x64/sym/symtab.o
	$(LINK) $(LDFLAGS) -machine:x64 -map:build/BOOTX64.map -out:$@ $(X64_OBJ) build/x64/sym/symtab.o

# ---- 64-bit ARM (QEMU virt, Xiaomi Mi A1): build/arm64/Image, a Linux-style arm64 kernel image ----
ARM_SHARED := src/kernel/rt.c src/kernel/vfs.c src/kernel/sound.c src/kernel/time.c \
              src/ui/gfx.c src/ui/shell.c src/ui/clientwin.c src/ui/fontdata.c src/ui/osk.c \
              src/apps/clock.c src/apps/sketch.c src/apps/files.c src/apps/system.c src/apps/settings.c src/apps/life.c \
              src/apps/lab.c src/apps/terminal.c src/apps/wifi.c src/apps/browser.c src/apps/bluetooth.c src/apps/html.c \
              src/net/wifilog.c src/net/crypto.c src/net/crypto_tls.c src/net/tls.c src/net/http.c src/net/net.c src/net/tcp.c \
              src/net/wlan.c src/net/netstack.c src/drivers/hidparse.c src/drivers/hidmouse.c
ARM_SRC    := $(wildcard src/arch/arm64/*.c)
ARM_OBJ    := $(ARM_SHARED:src/%.c=build/arm64/%.o) $(ARM_SRC:src/%.c=build/arm64/%.o) build/arm64/arch/arm64/boot.o build/arm64/arch/arm64/switch.o
ARM_CFLAGS := -target aarch64-none-elf -fpie -mno-outline-atomics -mstrict-align

build/arm64/kernel/time.o: CFLAGS += -DQRT_BUILD_EPOCH=$(shell git log -1 --format=%ct 2>/dev/null || echo 1767225600)ull
build/arm64/%.o: src/%.c $(wildcard src/*.h src/*/*.h src/*/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ARM_CFLAGS) -c $< -o $@
build/arm64/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) -target aarch64-none-elf -c $< -o $@
# Linux's drivers inside the kernel, when ports/lkl-arm64.sh has built LKL
ARM_LKL := $(wildcard build/arm64/lkl/lkl.o)
ifneq ($(ARM_LKL),)
build/arm64/arch/arm64/linux.o: CFLAGS += -DQRT_LKL -Ibuild/arm64/lkl/include
build/arm64/arch/arm64/hal.o: CFLAGS += -DQRT_LKL
build/arm64/arch/arm64/lwifi.o: CFLAGS += -DQRT_LKL
build/arm64/arch/arm64/lsound.o: CFLAGS += -DQRT_LKL
build/arm64/arch/arm64/linux.o build/arm64/arch/arm64/hal.o: $(ARM_LKL)
ARM_LIBS := $(ARM_LKL) $(shell aarch64-linux-gnu-gcc -print-libgcc-file-name 2>/dev/null)
endif
build/arm64/qrt.elf: $(ARM_OBJ) src/arch/arm64/link.ld $(ARM_LKL)
	ld.lld -pie --no-dynamic-linker -z notext -T src/arch/arm64/link.ld -o $@ $(ARM_OBJ) $(ARM_LIBS)
build/arm64/Image: build/arm64/qrt.elf
	llvm-objcopy -O binary $< $@
arm64: build/arm64/Image

# Xiaomi Mi A1 (tissot): an Android boot image - Image.gz with the mainline device tree
# appended (as aboot wants it), a small ramdisk (a cpio archive: the file tree).
# Try it without flashing: fastboot boot build/arm64/qrt-mi-a1-boot.img
# the device tree: mainline Linux 6.12's msm8953-xiaomi-tissot.dts, compiled into
# src/arch/arm64/dts: postmarketOS's tissot device tree (msm8953-mainline, branch 6.12/main) -
# regenerate with: make dts-tissot LINUX_DTS=<msm8953-mainline> LINUX_INC=<any Linux 6.12 tree>
build/arm64/tissot.dtb: src/arch/arm64/dts/msm8953-xiaomi-tissot.dtb
	@mkdir -p $(dir $@)
	cp $< $@
dts-tissot:
	@test -n "$(LINUX_DTS)" || { echo "set LINUX_DTS to a Linux 6.12 tree"; exit 1; }
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $(LINUX_DTS)/include $(if $(LINUX_INC),-I $(LINUX_INC)/include) -I $(LINUX_DTS)/arch/arm64/boot/dts/qcom \
	    -I $(LINUX_DTS)/scripts/dtc/include-prefixes $(LINUX_DTS)/arch/arm64/boot/dts/qcom/msm8953-xiaomi-tissot.dts | dtc -q -I dts -O dtb -o src/arch/arm64/dts/msm8953-xiaomi-tissot.dtb -
build/arm64/ramdisk.cpio: $(shell find src/arch/arm64/rootfs -type f 2>/dev/null)
	$(PYTHON) tools/mkcpio.py src/arch/arm64/rootfs $@
build/arm64/qrt-mi-a1-boot.img: build/arm64/Image build/arm64/tissot.dtb build/arm64/ramdisk.cpio
	gzip -9nc build/arm64/Image > build/arm64/Image.gz
	cat build/arm64/Image.gz build/arm64/tissot.dtb > build/arm64/Image.gz-dtb
	$(PYTHON) tools/mkbootimg.py build/arm64/Image.gz-dtb build/arm64/ramdisk.cpio $@ "qrt"
mi-a1: build/arm64/qrt-mi-a1-boot.img

# Linux programs shipped in /bin (run by the native kernel's Linux layer)
BUSYBOX ?= /bin/busybox
# Rust for QRT (needs: rustup target add x86_64-unknown-linux-musl); skipped without it
RUST_MUSL := $(shell d=$$(rustc --print sysroot 2>/dev/null)/lib/rustlib/x86_64-unknown-linux-musl; [ -d "$$d" ] && echo yes)
RUST_HELLO := $(if $(RUST_MUSL),build/rootfs/bin/rust-hello)
# Ladybird, built natively for QRT by ports/ladybird/build.sh; shipped when it has been built
LADYBIRD := $(if $(wildcard build/ladybird/bin/js),build/rootfs/bin/js build/rootfs/share/tests/js-test.js)
# the browser (one program: it and its helper processes), its resources, fonts, fontconfig
LADYBIRD += $(if $(wildcard build/ladybird/bin/Ladybird),build/rootfs/bin/ladybird $(if $(wildcard build/ladybird/bin/cranelift-compiler),build/rootfs/bin/cranelift-compiler) build/rootfs/share/Lagom/.stamp \
            build/rootfs/share/fonts/.stamp build/rootfs/etc/fonts/fonts.conf build/rootfs/share/tests/page.html build/rootfs/share/tests/fps.html build/rootfs/share/tests/functions.html build/rootfs/share/tests/video.html build/rootfs/share/tests/video.webm build/rootfs/share/tests/render.sh \
            build/rootfs/etc/ssl/certs/ca-certificates.crt)
FONT_DIRS := /usr/share/fonts/truetype/liberation /usr/share/fonts/truetype/dejavu
# the Linux driver host: ports/lkl.sh builds Linux as a library (build/linuxdrv/lkl.o),
# then src/linuxdrv is linked with it into /bin/linuxdrv
LINUXDRV := $(if $(wildcard build/linuxdrv/lkl.o),build/rootfs/bin/linuxdrv build/rootfs/etc/linuxdrv.conf)
LINUXDRV_SRC := src/linuxdrv/linuxdrv.c src/linuxdrv/lkl/iomem.c src/linuxdrv/lkl/utils.c
build/linuxdrv/linuxdrv: $(LINUXDRV_SRC) src/linuxdrv/lkl/iomem.h build/linuxdrv/lkl.o
	musl-gcc -O2 -Wall -Wno-format-truncation -include sys/types.h -Ibuild/linuxdrv/include -static -Wl,-z,noexecstack \
	    -o $@ $(LINUXDRV_SRC) build/linuxdrv/lkl.o -lpthread
	strip $@
build/rootfs/etc/linuxdrv.conf: src/linuxdrv/linuxdrv.conf
	@mkdir -p $(dir $@)
	cp $< $@
build/rootfs/bin/linuxdrv: build/linuxdrv/linuxdrv
	@mkdir -p $(dir $@)
	cp $< $@

ROOTFS := $(LINUXDRV) build/rootfs/bin/hello build/rootfs/bin/hello-musl build/rootfs/bin/busybox \
          build/rootfs/bin/dynhello build/rootfs/bin/threads build/rootfs/bin/cxx build/rootfs/bin/procs build/rootfs/bin/signals build/rootfs/bin/memory build/rootfs/bin/events build/rootfs/bin/native-test build/rootfs/bin/play build/rootfs/bin/hello-window build/rootfs/bin/cxx-test $(RUST_HELLO) $(LADYBIRD) build/rootfs/lib64/ld-linux-x86-64.so.2

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

build/rootfs/bin/signals: tests/linux/signals.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -s -o $@ $< -lpthread

build/rootfs/bin/memory: tests/linux/memory.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -s -o $@ $<

# The QRT SDK (musl for QRT's native system calls, libqrt, qrt-cc) and native programs
SDK_SRC := sdk/build.sh sdk/patch_musl.py sdk/syscalls.txt sdk/musl-1.2.6.tar.gz sdk/libqrt/qrt.c sdk/libqrt/qrt.h sdk/libqrt/genfont.py
QRT_CC := build/sdk/bin/qrt-cc
build/sdk/.stamp: $(SDK_SRC)
	sh sdk/build.sh build/sdk
	touch $@

sdk: build/sdk/.stamp

# C++ for QRT: LLVM 20's libc++, libc++abi, libunwind, compiler-rt (sdk/build-cxx.sh)
QRT_CXX := build/sdk/bin/qrt-c++
build/sdk/.cxxstamp: sdk/build-cxx.sh build/sdk/.stamp
	sh sdk/build-cxx.sh build/sdk
	touch $@

build/rootfs/bin/cxx-test: sdk/examples/cxx-test.cpp build/sdk/.cxxstamp
	@mkdir -p $(dir $@)
	$(QRT_CXX) -std=c++23 -O2 -s -o $@ $<

build/rootfs/bin/js: build/ladybird/bin/js
	@mkdir -p $(dir $@)
	llvm-strip-20 -o $@ $<

build/rootfs/bin/ladybird: build/ladybird/bin/Ladybird
	@mkdir -p $(dir $@)
	llvm-strip-20 -o $@ $<

# WebAssembly's compiler (Cranelift): Ladybird runs it next to itself, without it pages' wasm is interpreted
build/rootfs/bin/cranelift-compiler: build/ladybird/bin/cranelift-compiler
	@mkdir -p $(dir $@)
	llvm-strip-20 -o $@ $<

build/rootfs/share/Lagom/.stamp: build/ladybird/bin/Ladybird
	rm -rf build/rootfs/share/Lagom; mkdir -p build/rootfs/share
	cp -r build/ladybird/share/Lagom build/rootfs/share/Lagom
	touch $@

# Liberation (SIL OFL; metric-compatible with Arial, Times New Roman, Courier New) and
# DejaVu (free licence, see share/fonts/README.txt), from the build machine
build/rootfs/share/fonts/.stamp: tools/fonts.conf
	@mkdir -p $(dir $@)
	for d in $(FONT_DIRS); do cp $$d/*.ttf $(dir $@); done
	printf 'Liberation fonts: SIL Open Font License 1.1 (github.com/liberationfonts).\nDejaVu fonts: Bitstream Vera licence with DejaVu changes in the public domain (dejavu-fonts.github.io).\n' > $(dir $@)README.txt
	touch $@

# Mozilla's root certificates for HTTPS (curl reads them here), as packaged by certifi
# (PyPI) - not the build machine's own store
CERTIFI_VERSION := 2026.7.22
build/rootfs/etc/ssl/certs/ca-certificates.crt:
	@mkdir -p $(dir $@) build/dl
	[ -f build/dl/certifi-$(CERTIFI_VERSION).whl ] || pip download -q --no-deps -d build/dl/certifi certifi==$(CERTIFI_VERSION) && cp build/dl/certifi/certifi-$(CERTIFI_VERSION)-*.whl build/dl/certifi-$(CERTIFI_VERSION).whl
	python3 -c "import zipfile,sys; sys.stdout.buffer.write(zipfile.ZipFile('build/dl/certifi-$(CERTIFI_VERSION).whl').read('certifi/cacert.pem'))" > $@

build/rootfs/etc/fonts/fonts.conf: tools/fonts.conf
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/render.sh: tests/ladybird/render.sh
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/page.html: tests/ladybird/page.html
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/fps.html: tests/ladybird/fps.html
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/functions.html: tests/ladybird/functions.html
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/video.html: tests/ladybird/video.html
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/video.webm: tests/ladybird/video.webm
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/share/tests/js-test.js: tests/ladybird/js-test.js
	@mkdir -p $(dir $@)
	cp $< $@

build/rootfs/bin/native-test: sdk/examples/native-test.c build/sdk/.stamp
	@mkdir -p $(dir $@)
	$(QRT_CC) -O2 -s -o $@ $<

build/rootfs/bin/play: sdk/examples/play.c build/sdk/.stamp
	@mkdir -p $(dir $@)
	$(QRT_CC) -O2 -s -o $@ $< -lm

build/rootfs/bin/hello-window: sdk/examples/hello-window.c build/sdk/.stamp
	@mkdir -p $(dir $@)
	$(QRT_CC) -O2 -s -o $@ $< -lqrt

build/rootfs/bin/rust-hello: sdk/examples/rust-hello/src/main.rs sdk/examples/rust-hello/Cargo.toml build/sdk/.stamp
	@mkdir -p $(dir $@)
	cd sdk/examples/rust-hello && CARGO_TARGET_DIR=$(CURDIR)/build/rust-hello $(CURDIR)/build/sdk/bin/qrt-cargo build --release -q
	cp build/rust-hello/x86_64-unknown-linux-musl/release/rust-hello $@

src/arch/x64/native_sys.h: sdk/syscalls.txt tools/gen_native_sys.py
	$(PYTHON) tools/gen_native_sys.py $< $@

build/rootfs/bin/events: tests/linux/events.c
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

build/qrt.img: build/BOOTX64.EFI tools/mkimage.sh $(wildcard image/*) $(wildcard firmware/*) $(ROOTFS)
	tools/mkimage.sh $@ build/BOOTX64.EFI build/rootfs

run run64: build/qrt.img
	tools/run-qemu.sh x64

test: build/qrt.img
	$(PYTHON) tools/qemu-test.py x64

check:
	@mkdir -p build
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_hid tests/test_hid.c && build/test_hid
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_crypto tests/test_crypto.c && build/test_crypto
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_battery tests/test_battery.c && build/test_battery
	$(HOSTCC) -Wall -Wextra -O1 -Itests/sof -o build/test_speaker tests/test_speaker.c && build/test_speaker
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_ish tests/test_ish.c && build/test_ish
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -O1 -fshort-wchar -o build/test_wlan tests/test_wlan.c && build/test_wlan
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_http tests/test_http.c && build/test_http
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_html tests/test_html.c && build/test_html
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O1 -fshort-wchar -o build/test_gpu tests/test_gpu.c -lm && build/test_gpu
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O1 -fshort-wchar -o build/test_display tests/test_display.c -lm && build/test_display
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_mouse tests/test_mouse.c && build/test_mouse
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O1 -fshort-wchar -o build/test_bt tests/test_bt.c -lm && build/test_bt   # FFMPEG=...: the A2DP stream decoded too
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_sound tests/test_sound.c -lm && build/test_sound
	$(HOSTCC) -Wall -Wextra -O1 -o build/test_sbc tests/test_sbc.c -lm && build/test_sbc     # FFMPEG=path/to/ffmpeg: decoded and compared too

# TLS 1.3 client against a local OpenSSL server (needs openssl and python3)
check-tls:
	@mkdir -p build
	$(HOSTCC) -Wall -Wextra -Wno-unused-function -O2 -o build/test_tls tests/test_tls.c && tools/tls-test.sh

clean:
	rm -rf build

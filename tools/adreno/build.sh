#!/bin/sh
# Builds qrtcc against a Mesa source tree and its meson build directory, then compiles
# QRT's GPU kernels (src/arch/arm64/gpu/NAME.comp) into src/arch/arm64/gpu_kernels.h.
#
#   tools/adreno/build.sh MESA_SRC MESA_BUILD GLSLANG
#
# Mesa: configured with -Dgallium-drivers=freedreno (everything else off) and built for
# the targets below; glslang: any recent glslang (GLSL -> SPIR-V).  Not part of the
# normal build - the header it writes is committed.
set -e
M=$1; B=$2; G=$3
[ -d "$M/src" ] && [ -d "$B/src" ] && [ -x "$G" ] || { echo "usage: $0 MESA_SRC MESA_BUILD GLSLANG"; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${QRTCC_KEEP:-${TMPDIR:-/tmp}/qrtcc.$$}
mkdir -p "$out"
libs="src/freedreno/ir3/libfreedreno_ir3.a src/compiler/spirv/libvtn.a src/freedreno/isa/libir3encode.a
      src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/freedreno/common/libfreedreno_common.a
      src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a"
ninja -C "$B" $libs >/dev/null
# Mesa's own flags for ir3 (defines, include paths), from its compile database
I=$(python3 - "$B" <<'PY'
import json, os, shlex, sys
b = sys.argv[1]
for e in json.load(open(os.path.join(b, "compile_commands.json"))):
    if e["file"].endswith("freedreno/ir3/ir3_nir.c"):
        out = []
        for a in shlex.split(e["command"]):
            if a.startswith("-I"):
                d = a[2:]
                out.append("-I" + (d if os.path.isabs(d) else os.path.normpath(os.path.join(e["directory"], d))))
            elif a.startswith("-D") or a.startswith("-std"):
                out.append(shlex.quote(a))
        print(" ".join(out))
        break
PY
)
L=""
for l in $libs; do L="$L $B/$l"; done
eval gcc -O1 $I -std=gnu11 "$here/qrtcc.c" -Wl,--start-group $L -Wl,--end-group -lm -lpthread -ldl -lz -lstdc++ -fopenmp -o \"$out/qrtcc\"
args=""
for f in "$root"/src/arch/arm64/gpu/*.comp; do
    n=$(basename "$f" .comp)
    "$G" -V --target-env vulkan1.1 "$f" -o "$out/$n.spv" >/dev/null
    args="$args $n $out/$n.spv"
done
"$out/qrtcc" "$root/src/arch/arm64/gpu_kernels.h" $args
[ -n "$QRTCC_KEEP" ] || rm -rf "$out"

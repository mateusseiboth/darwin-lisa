#!/bin/sh
# The sources of the UniXNU GPU stack for lisa (docs/gpu-driver-rewrite.md, T0b/T1), from their upstreams, at the commits
# dada's gpu2/ patches apply to, with dada's patches and then ours (this directory's *-lisa.patch) applied:
#   Mesa 85fda3b (gitlab.freedesktop.org)            + dada mesa-85fda3b-dada.patch       + mesa-lisa.patch
#   metal2vulkan 720ef1c (github steelbrain)         + dada metal2vulkan-720ef1c-dada.patch + metal2vulkan-lisa.patch
#   AdrenoMetal c6d29ca (github UniXNU)              + dada nativemetal-dada.patch        + nativemetal-lisa.patch
#   AdrenoPM4, NativeMobileDisplay (github UniXNU, for two headers), NativeAdrenoABI.h of Adreno830 (github Shad0w23333)
#   dada's patches: github superboyyy/hakios-arm64e-dada f1ae906 gpu2/patches
#   glslang 16.0.0 (github KhronosGroup), built here by hand (this host has no cmake): Mesa needs it for its fp64 and BVH shaders
# Each tree is a git checkout whose commit "dada" holds the upstream pin plus dada's patch; ours are the working tree on top
# (git -C src/mesa diff dada > mesa-lisa.patch regenerates the patch). metal2vulkan is checked out sparse: its 1.6 GB
# validation corpus is left out.
#   fetch-src.sh   -> $UNIXNU/src/*, $UNIXNU/tools/glslangValidator
set -eu
UNIXNU=${UNIXNU:-$HOME/Dev/mobile/userland-26/gpu-build/unixnu}
HERE=$(cd "$(dirname "$0")" && pwd)
S=$UNIXNU/src
mkdir -p "$S" "$UNIXNU/tools" "$UNIXNU/host-deps"
cd "$S"
clone_at() {   # clone_at URL DIR COMMIT: a partial clone checked out at COMMIT
	[ -d "$2/.git" ] || git clone -q --filter=blob:none --no-checkout "$1" "$2"
	git -C "$2" checkout -q "$(git -C "$2" rev-parse "$3")"
}
[ -d dada/.git ] || git clone -q --filter=blob:none https://github.com/superboyyy/hakios-arm64e-dada.git dada
git -C dada checkout -q f1ae906fcc43ba8f763663257724ba938fb820ab
DP=$S/dada/gpu2/patches

if [ ! -d metal2vulkan/.git ]; then
	git clone -q --filter=blob:none --no-checkout https://github.com/steelbrain/metal2vulkan.git metal2vulkan
	git -C metal2vulkan sparse-checkout init --no-cone
	printf '%s\n' '/*' '!/crates/metal2vulkan-validation/fixtures/' '!/crates/metal2vulkan-validation/corpus/' \
		'!/crates/metal2vulkan-conformance/' '/crates/metal2vulkan-conformance/Cargo.toml' '/crates/metal2vulkan-conformance/src/' \
		'/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll' \
		'/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll' > metal2vulkan/.git/info/sparse-checkout
fi
clone_at https://github.com/UniXNU/AdrenoMetal.git adrenometal c6d29ca
clone_at https://github.com/steelbrain/metal2vulkan.git metal2vulkan 720ef1ce9ae2a2068999a8675717418b9416283e
[ -d adrenopm4/.git ] || git clone -q --depth 1 https://github.com/UniXNU/AdrenoPM4.git adrenopm4
[ -d NativeMobileDisplay/.git ] || git clone -q --depth 1 https://github.com/UniXNU/NativeMobileDisplay.git NativeMobileDisplay
if [ ! -d mesa/.git ]; then
	git init -q mesa
	git -C mesa remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
	git -C mesa fetch -q --depth 1 origin 85fda3b36405bcaea8bffc64aa4f2651dc90770d
	git -C mesa checkout -q FETCH_HEAD
fi
mkdir -p nativeadreno
[ -f nativeadreno/NativeAdrenoABI.h ] || curl -sSfL -o nativeadreno/NativeAdrenoABI.h \
	https://raw.githubusercontent.com/Shad0w23333/Adreno830/1a1321056ffc5428ae925f4680093718b46ca7c2/NativeAdrenoABI.h

# dada's patch as the commit "dada", then ours in the working tree
dada() {       # dada DIR PATCH [LISA]
	if ! git -C "$1" rev-parse -q --verify dada >/dev/null; then
		git -C "$1" apply "$DP/$2"
		git -C "$1" add -A
		git -C "$1" -c user.name=build -c user.email=build@localhost commit -qm "dada gpu2 $2"
		git -C "$1" tag dada
	fi
	if [ -n "${3:-}" ] && git -C "$1" diff --quiet; then git -C "$1" apply "$HERE/$3"; fi
}
dada mesa mesa-85fda3b-dada.patch mesa-lisa.patch
dada metal2vulkan metal2vulkan-720ef1c-dada.patch metal2vulkan-lisa.patch
dada adrenometal nativemetal-dada.patch nativemetal-lisa.patch

# glslangValidator, by hand
G=$UNIXNU/host-deps/glslang
if [ ! -x "$UNIXNU/tools/glslangValidator" ]; then
	[ -d "$G/.git" ] || git clone -q --depth 1 --branch 16.0.0 https://github.com/KhronosGroup/glslang.git "$G"
	cd "$G"
	mkdir -p gen/glslang obj
	python3 -I build_info.py . -i build_info.h.tmpl -o gen/glslang/build_info.h
	python3 -I gen_extension_headers.py -i glslang/ExtensionHeaders -o gen/glslang/glsl_intrinsic_header.h
	ls glslang/MachineIndependent/*.cpp glslang/MachineIndependent/preprocessor/*.cpp glslang/GenericCodeGen/*.cpp \
		glslang/OSDependent/Unix/*.cpp glslang/ResourceLimits/*.cpp SPIRV/*.cpp StandAlone/StandAlone.cpp | grep -v SpvTools.cpp |
		xargs -P"$(nproc)" -I{} sh -c 'g++ -O2 -std=c++17 -w -DENABLE_SPIRV=1 -DENABLE_OPT=0 -DGLSLANG_OSINCLUDE_UNIX -I. -Igen \
			-Iglslang/Include -c {} -o obj/$(echo {} | tr / _).o'
	g++ -o "$UNIXNU/tools/glslangValidator" obj/*.o -lpthread
fi
"$UNIXNU/tools/glslangValidator" --version | head -1
echo "fetch-src: done"

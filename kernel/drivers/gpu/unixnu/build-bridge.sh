#!/bin/sh
# The native Metal bridge (AdrenoMetal's bridge/NativeMetalBridge.rs: the metal2vulkan runtime behind a C ABI) as a static library
# for lisa's iOS 26: dada's gpu2/build_bridge.sh, plain arm64 (aarch64-apple-ios, rustup's std; the plug-in is relabelled arm64e
# afterwards as HakiosMetal is: this userland has no pointer authentication). Rust 1.98.1 from rustup, not Fedora's (no iOS std).
# Sources: $UNIXNU/src/metal2vulkan (720ef1c + dada's patch), $UNIXNU/src/adrenometal (c6d29ca + dada's patch + nativemetal-lisa.patch).
# metal2vulkan is GPL-2.0-or-later OR LGPL-3.0-or-later: the bundle that links this library ships with its source (docs/gpu-driver-rewrite.md).
#   build-bridge.sh          -> $UNIXNU/out/bridge/ios/libnative_metal_bridge.a
set -eu
M=$HOME/Dev/mobile
UNIXNU=${UNIXNU:-$M/userland-26/gpu-build/unixnu}
SDK=${SDK:-$M/research/iOS-SDKs/iPhoneOS26.5.sdk}
M2V=$UNIXNU/src/metal2vulkan
NM=$UNIXNU/src/adrenometal
B=$UNIXNU/build/bridge
OUT=$UNIXNU/out/bridge/ios
CARGO="$HOME/.cargo/bin/cargo +1.98.1"
mkdir -p "$B" "$OUT"
llvm-as -o "$B/runtime_examples.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
llvm-as -o "$B/function_constant.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
cat > "$B/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$NM/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$M2V/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$M2V/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10" }
[profile.release]
panic = "unwind"
debug = 1
EOF
# the translator source keys the shader cache (PrimeShaders.h): its pin and every change on top of it
RID=$( (git -C "$M2V" rev-parse HEAD; git -C "$M2V" diff HEAD) | sha256sum | cut -c1-32)
export NMB_RUNTIME_ID=$RID NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air
export IPHONEOS_DEPLOYMENT_TARGET=16.0
# C that a crate's build script compiles for the target (cc-rs): the SDK's headers, this host's clang
export CC_aarch64_apple_ios=clang AR_aarch64_apple_ios=llvm-ar
export CFLAGS_aarch64_apple_ios="-target arm64-apple-ios16.0 -isysroot $SDK"
export SDKROOT=$SDK
cd "$B"
$CARGO build --release --manifest-path "$B/Cargo.toml" --target aarch64-apple-ios --target-dir "$UNIXNU/build/m2v-target"
cp "$UNIXNU/build/m2v-target/aarch64-apple-ios/release/libnative_metal_bridge.a" "$OUT/"
echo "runtime id $RID"
ls -l "$OUT/libnative_metal_bridge.a"

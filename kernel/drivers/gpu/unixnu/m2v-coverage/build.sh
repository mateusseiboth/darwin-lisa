#!/bin/sh
# m2v-coverage for the host (T0b): metal2vulkan's runtime from the source tree fetch-src.sh made (dada's 720ef1c and
# its patch), turnip from build-host-turnip.sh. Rust from rustup (1.98.1), not Fedora's.
#   build.sh -> $UNIXNU/build/m2v-coverage/release/m2v-coverage, m2v-formats (formats.rs: text backing formats, drawn)
set -eu
UNIXNU=${UNIXNU:-$HOME/Dev/mobile/userland-26/gpu-build/unixnu}
HERE=$(cd "$(dirname "$0")" && pwd)
B=$UNIXNU/build/m2v-coverage-crate
mkdir -p "$B"
cat > "$B/Cargo.toml" <<TOML
[package]
name = "m2v-coverage"
version = "0.1.0"
edition = "2021"
publish = false
[workspace]
[[bin]]
name = "m2v-coverage"
path = "$HERE/main.rs"
[[bin]]
name = "m2v-formats"
path = "$HERE/formats.rs"
[dependencies]
metal2vulkan = { path = "$UNIXNU/src/metal2vulkan/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$UNIXNU/src/metal2vulkan/crates/metal2vulkan-runtime" }
[profile.release]
debug = 0
TOML
cd "$B"
"$HOME/.cargo/bin/cargo" +1.98.1 build --release --target-dir "$UNIXNU/build/m2v-coverage"
ls -l "$UNIXNU/build/m2v-coverage/release/m2v-coverage" "$UNIXNU/build/m2v-coverage/release/m2v-formats"

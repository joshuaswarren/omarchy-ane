#!/usr/bin/env bash
set -euo pipefail
src=/home/admin/aurora-build/wt-dart-diag
kit=/var/tmp/opencode/h17-kernel-kit
out=/home/admin/aurora-build/h17-publication-kernel
export RUSTUP_HOME=/var/tmp/opencode/h17-rustup
export CARGO_HOME=/var/tmp/opencode/h17-cargo
export PATH="/usr/lib/llvm18/bin:$kit/private/usr/bin:$CARGO_HOME/bin:$PATH"
export ARCH=arm64
export LIBCLANG_PATH=/usr/lib/llvm18/lib
export KBUILD_BUILD_USER=3j27 KBUILD_BUILD_HOST=h17-offline-build
test "$(git -C "$src" rev-parse HEAD)" = 4fdd0fc5c931ef9f14474942b37c3d451e97fc04
git -C "$src" diff --src-prefix=i/ --dst-prefix=w/ HEAD -- drivers/iommu/apple-dart.c | cmp - /home/admin/omarchy-ane/receipts/2026-11-04-j700-h17-dart-publication-diagnostic/publication-diagnostic.patch
test "$(git -C "$src" diff HEAD --name-only)" = drivers/iommu/apple-dart.c
test -z "$(git -C "$src" ls-files --others --exclude-standard)"
(cd "$kit" && sha256sum -c <<'HASHES'
b95c0f0ddf106f3861e0e62a746af6a881fb8e0f356a892961f9e86062877400  configure.sh
84a03d5fea19cae243c89f50c1736c639622d25d4d1de8d7d53ebd8f603c0f62  bindgen-kernel.py
443a9c4613172ea352f49b114818a1d75ab048ec4bd2d75b388a7ab3a8a1f046  rust-toolchain.toml
aca7420a18139474da62a81c77fc6e5c3f25001f872b6e11e18605cd892aa4c3  validate-config.py
98f76a885d81711013aad47e2ee13f000923191e43321738ec8f381690bf5160  config
HASHES
)
test ! -e "$out"
mkdir -p "$out" "$kit/private" "$CARGO_HOME/bin"
if [[ ! -e $CARGO_HOME/bin/rustup ]]; then
  cp /home/admin/.cargo/bin/rustup "$CARGO_HOME/bin/rustup"
fi
for proxy in cargo rustc rustdoc rustfmt cargo-fmt; do
  if [[ ! -e $CARGO_HOME/bin/$proxy ]]; then
    ln -s rustup "$CARGO_HOME/bin/$proxy"
  fi
done
bsdtar -xf /var/cache/pacman/pkg/bc-1.08.2-1-aarch64.pkg.tar.xz -C "$kit/private" usr/bin/bc
bc --version
rustup toolchain install 1.85.0 --profile minimal --component rust-src
rustup toolchain install 1.99.0 --profile minimal --component rustfmt
export RUSTFMT
RUSTFMT="$(rustup which --toolchain 1.99.0 rustfmt)"
RUSTUP_TOOLCHAIN=1.99.0 python3 "$src/tools/aurora-bindgen/build.py" --output-dir "$kit/private-bindgen" --jobs 2
export AURORA_BINDGEN_REAL="$kit/private-bindgen/rust-bindgen-0.73.2/target/release/bindgen"
export RUSTUP_TOOLCHAIN=1.85.0
{
  git -C "$src" rev-parse HEAD
  git -C "$src" diff -- drivers/iommu/apple-dart.c | sha256sum
  sha256sum "$src/drivers/iommu/apple-dart.c" "$kit"/configure.sh "$kit"/bindgen-kernel.py "$kit"/config
  /usr/lib/llvm18/bin/clang --version
  /usr/lib/llvm18/bin/ld.lld --version
  rustup run 1.85.0 rustc -vV
  rustup run 1.99.0 rustc -vV
  "$RUSTFMT" --version
  "$AURORA_BINDGEN_REAL" --version
} > "$out/build-identity.txt"
chmod u+x "$kit/bindgen-kernel.py"
_make() {
  nice -n 10 make -C "$src" LLVM=/usr/lib/llvm18/bin/ \
    BINDGEN="$kit/bindgen-kernel.py" LOCALVERSION= "$@"
}
cd "$src"
source "$kit/configure.sh"
configure_kernel "$kit" "$out"
printf '%s\n' '-14-1-h17pub' > "$out/localversion.10-pkgrel"
_make O="$out" -s kernelrelease > "$out/diagnostic-release.txt"
cat "$out/diagnostic-release.txt"
test "$(cat "$out/diagnostic-release.txt")" = 7.2.11-14-1-h17pub-ARCH
test ! -e /lib/modules/7.2.11-14-1-h17pub-ARCH
diff -u /lib/modules/7.2.11-14-1-ARCH/build/.config "$out/.config" > "$out/config-vs-installed.diff" || test "$?" = 1
_make O="$out" -j2 Image modules dtbs
sha256sum "$out/arch/arm64/boot/Image" > "$out/Image.sha256"
find "$out" -type f \( -name '*.ko' -o -name '*.dtb' \) -print0 | sort -z | xargs -0 -r sha256sum > "$out/modules-dtbs.sha256"
echo 'BUILD_COMPLETE: artifacts only; nothing installed or armed'

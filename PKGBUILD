# Maintainer: local

pkgname=ee-eq-cli-git
pkgver=0.2.9.2
pkgrel=1
pkgdesc='Minimal headless EasyEffects-compatible EQ preset loader for PipeWire/LV2'
arch=(x86_64 aarch64)
url='https://github.com/wwmm/easyeffects'
license=('GPL3')
depends=(
  'pipewire'
  'lilv'
  'nlohmann-json'
  'libsndfile'
  'zita-convolver'
)
makedepends=(
  'cmake'
  'ninja'
  'pkgconf'
)
optdepends=(
  'lsp-plugins: LSP LV2 equalizer plugin required at runtime'
)
conflicts=(ee-eq-cli)
provides=(ee-eq-cli)
source=()
sha256sums=()

build() {
    cd "${startdir}"

    cmake \
      -B build \
      -S "${startdir}" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr

    cmake --build build
  }

  package() {
    cd "${startdir}"

    install -Dm755 build/ee-eq-cli "${pkgdir}/usr/bin/ee-eq-cli"
    install -Dm644 README.md "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}

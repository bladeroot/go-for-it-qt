# Maintainer: Yurii Myronchuk <bladeroot@gmail.com>
pkgname=go-for-it-qt
pkgver=2.0.1
pkgrel=1
pkgdesc="A stylish to-do list with built-in productivity timer, SQLite storage, TUI, and multi-tracker integrations (Qt 6)"
arch=('x86_64' 'aarch64')
url="https://github.com/bladeroot/go-for-it-qt"
license=('GPL-3.0-or-later')
depends=(
  'gcc-libs'
  'glibc'
  'hicolor-icon-theme'
  'qt6-base'
  'qt6-declarative'
  'qt6-multimedia'
)
makedepends=(
  'cmake'
  'git'
  'ninja'
)
provides=('go-for-it-qt')
conflicts=('go-for-it-qt-git')
source=("git+${url}.git#branch=main")
sha256sums=('SKIP')

build() {
  cmake -B build -S "${pkgname}" \
    -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=ON \
    -Wno-dev
  cmake --build build
}

check() {
  QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
}

package() {
  DESTDIR="${pkgdir}" cmake --install build
}

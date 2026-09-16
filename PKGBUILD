# Maintainer: yappologistic <262229790+yappologistic@users.noreply.github.com>
pkgname=nala
pkgver=1.0.0
pkgrel=1
pkgdesc="A desktop companion for Hyprland that follows the cursor and takes its colours from the wallpaper"
arch=('x86_64' 'aarch64')
url="https://github.com/yappologistic/Nala"
license=('MIT')
depends=('qt6-base' 'qt6-declarative' 'layer-shell-qt')
makedepends=('cmake' 'ninja' 'qt6-shadertools')
optdepends=('hyprland: cursor tracking, workspace and fullscreen awareness'
            'noctalia-shell: wallpaper theming and notification badges')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  cmake -S "Nala-$pkgver" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

check() {
  QT_QPA_PLATFORM=offscreen ./build/nala --self-test
}

package() {
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 "Nala-$pkgver/LICENSE" \
    "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}

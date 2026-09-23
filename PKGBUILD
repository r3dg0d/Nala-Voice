# Maintainer: yappologistic <262229790+yappologistic@users.noreply.github.com>
# Contributor: r3dg0d <192937334+r3dg0d@users.noreply.github.com>
pkgname=nala
pkgver=1.2.0
pkgrel=1
pkgdesc="A desktop companion for Hyprland with a local voice assistant and screen memory"
arch=('x86_64' 'aarch64')
url="https://github.com/r3dg0d/Nala-Voice"
license=('MIT')
depends=('qt6-base' 'qt6-declarative' 'qt6-multimedia' 'layer-shell-qt' 'onnxruntime')
makedepends=('cmake' 'ninja' 'qt6-shadertools')
optdepends=('hyprland: cursor tracking, window control, screen memory'
            'noctalia-shell: wallpaper theming and notification badges'
            'whisper.cpp: speech recognition'
            'ollama: a local language model (or any OpenAI-compatible server)'
            'grim: screenshots for vision and screen memory'
            'wtype: typing and key presses for computer use'
            'ydotool: clicking and scrolling for computer use')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  cmake -S "Nala-Voice-$pkgver" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

check() {
  QT_QPA_PLATFORM=offscreen ./build/nala --self-test
  QT_QPA_PLATFORM=offscreen HYPRLAND_INSTANCE_SIGNATURE= ./build/nala-assistant-tests
}

package() {
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 "Nala-Voice-$pkgver/LICENSE" \
    "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}

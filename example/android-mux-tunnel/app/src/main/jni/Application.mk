# Android 5.0 (API 21) is the minimum supported version.
APP_PLATFORM := android-21
# only the two libraries the app loads (hev also defines an executable module we do not need)
APP_MODULES := muxtunnel hev-socks5-tunnel
APP_STL := c++_static
APP_OPTIM := release
APP_CFLAGS := -O2
APP_CPPFLAGS := -std=c++17 -fexceptions -frtti
# 16 KB page size devices (Android 15+)
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true

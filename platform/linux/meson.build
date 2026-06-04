project('pinback-shell', 'c',
  version : '0.1.0',
  # buildtype=minsize (~-Os) + LTO for the smallest binary; the WebKit engine
  # is a system shared lib, so the executable itself is only our glue + stubs.
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=minsize', 'b_lto=true'])

# Both dependencies are resolved from the system via pkg-config — the WebKit
# engine is NOT bundled. Install the distro dev packages first:
#   Debian/Ubuntu : libgtk-4-dev libwebkitgtk-6.0-dev
#   Fedora        : gtk4-devel webkitgtk6.0-devel
#   Arch          : gtk4 webkitgtk-6.0
gtk    = dependency('gtk4')
webkit = dependency('webkitgtk-6.0')

executable('pinback-shell', 'main.c',
  dependencies : [gtk, webkit],
  # Drop unreferenced sections, over-linked libs, and symbols for a smaller binary.
  c_args : ['-ffunction-sections', '-fdata-sections'],
  link_args : ['-Wl,--gc-sections', '-Wl,--as-needed', '-s'],
  install : true)

# Brand icon + desktop entry, installed into the system theme so the window
# manager / launcher can show it (matched to the app id dev.pinback.shell).
install_data('dev.pinback.shell.desktop',
  install_dir : get_option('datadir') / 'applications')
foreach sz : ['16', '32', '48', '64', '128', '256', '512']
  install_data('icons' / 'hicolor' / (sz + 'x' + sz) / 'apps' / 'dev.pinback.shell.png',
    install_dir : get_option('datadir') / 'icons' / 'hicolor' / (sz + 'x' + sz) / 'apps')
endforeach


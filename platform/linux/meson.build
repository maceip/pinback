project('pinback-shell', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=2'])

# Both dependencies are resolved from the system via pkg-config — the WebKit
# engine is NOT bundled. Install the distro dev packages first:
#   Debian/Ubuntu : libgtk-4-dev libwebkitgtk-6.0-dev
#   Fedora        : gtk4-devel webkitgtk6.0-devel
#   Arch          : gtk4 webkitgtk-6.0
gtk    = dependency('gtk4')
webkit = dependency('webkitgtk-6.0')

executable('pinback-shell', 'main.c',
  dependencies : [gtk, webkit],
  install : true)

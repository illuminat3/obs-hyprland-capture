project(
  'obs-hyprland-capture',
  'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.60',
  default_options: ['c_std=gnu17', 'warning_level=2', 'buildtype=release'],
)

fs = import('fs')

libobs = dependency('libobs')
# Some distro packages ship libobs.pc pointing at ${prefix}/include while the
# headers live in ${prefix}/include/obs.
libobs_args = []
libobs_nested = libobs.get_variable(pkgconfig: 'includedir') / 'obs'
if fs.exists(libobs_nested / 'obs-module.h')
  libobs_args += '-I' + libobs_nested
endif
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable(pkgconfig: 'wayland_scanner'),
)
gbm = dependency('gbm')
libdrm = dependency('libdrm')
threads = dependency('threads')

protocol_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
protocols = [
  protocol_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  protocol_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
]

protocol_sources = []
foreach xml : protocols
  name = fs.stem(xml)
  protocol_sources += custom_target(
    name + '-client-header',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(
    name + '-protocol-code',
    input: xml,
    output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

shared_module(
  'obs-hyprland-capture',
  'src/plugin.c',
  'src/source.c',
  'src/capture.c',
  'src/enumerate.c',
  protocol_sources,
  name_prefix: '',
  c_args: libobs_args + ['-DPLUGIN_VERSION="' + meson.project_version() + '"'],
  dependencies: [libobs, wayland_client, gbm, libdrm, threads],
  install: true,
  install_dir: get_option('libdir') / 'obs-plugins',
)

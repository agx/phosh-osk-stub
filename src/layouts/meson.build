json_glib_utils = find_program('json-glib-validate', required: false)

layouts = [
  'am.json',
  'ara.json',
  'be.json',
  'bg.json',
  'by.json',
  'ca.json',
  'ch.json',
  'ch+fr.json',
  'cz.json',
  'de.json',
  'dk.json',
  'ee.json',
  'epo.json',
  'es+cat.json',
  'es.json',
  'fi.json',
  'fr.json',
  'gb.json',
  'ge.json',
  'gr.json',
  'hr.json',
  'hu.json',
  'id.json',
  'il.json',
  'in+bolnagri.json',
  'in+mal.json',
  'ir.json',
  'is.json',
  'it.json',
  'ke.json',
  'kg.json',
  'kh.json',
  'la.json',
  'latam.json',
  'lt.json',
  'lv.json',
  'mk.json',
  'mn.json',
  'my.json',
  'nl.json',
  'no.json',
  'ph.json',
  'pl.json',
  'pt.json',
  'ro.json',
  'rs.json',
  'ru.json',
  'se.json',
  'si.json',
  'sk.json',
  'th.json',
  'tr.json',
  'ua.json',
  'us.json',
  'us+dvorak.json',
  'vn.json',
  'za.json',
  # custom lauouts
  'terminal.json',
]

if json_glib_utils.found()
  foreach layout : layouts
    test(
      'Validate layout @0@'.format(layout),
      json_glib_utils,
      args: meson.current_source_dir() / layout,
    )
  endforeach
endif

info_builder = find_program(meson.project_source_root() / 'tools' / 'write-layout-info.py')
build_info = custom_target(
  'build-info',
  output: 'layouts.json',
  command: [
    info_builder,
    '--layouts=@0@'.format(meson.current_source_dir()),
    '--out=@OUTPUT@',
    varnam_dep.found() ? '--varnam' : '--no-varnam',
  ],
  depend_files: [layouts, info_builder.full_path()],
  env: {'LC_ALL': 'C'},
  install: true,
  install_dir: pkgdatadir,
)

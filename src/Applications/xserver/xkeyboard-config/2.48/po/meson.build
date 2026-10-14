if get_option('nls')
  i18n = import('i18n')
  i18n.gettext(
    f'xkeyboard-config-@xkc_version_major@',
    preset: 'glib',
    args: [
      '--keyword=_',
      '--keyword=N_',
      '--its=@0@/rules/xkb.its'.format(meson.project_source_root())]
  )
  # Legacy unversioned files
  if xkc_version_major != '2'
      error('These files are intended only while there is a single database format')
  endif
  i18n.gettext(
    'xkeyboard-config',
    preset: 'glib',
    args: [
      '--keyword=_',
      '--keyword=N_',
      '--its=@0@/rules/xkb.its'.format(meson.project_source_root())]
  )
endif

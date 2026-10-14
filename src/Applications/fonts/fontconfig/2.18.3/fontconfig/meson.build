fontconfig_h_conf = configuration_data()
fontconfig_h_conf.set('CACHE_VERSION', '@0@'.format(cacheversion))
fontconfig_h_conf.set('CACHE_MIN_COMPAT_VERSION', '@0@'.format(cachemincompat))
fontconfig_h_conf.set('CACHE_SNAP_VERSION', '@0@'.format(cachesnapversion))
fontconfig_h_conf.set('NEXT_CACHE_VERSION', '@0@'.format(nextcacheversion))
fontconfig_h = configure_file(
  output: 'fontconfig.h',
  input: 'fontconfig.h.in',
  configuration: fontconfig_h_conf,
)

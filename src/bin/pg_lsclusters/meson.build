# Kumar Server: pg_lsclusters

pg_lsclusters_sources = files(
  'pg_lsclusters.c',
)

if host_system == 'windows'
  pg_lsclusters_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_lsclusters',
    '--FILEDESC', 'pg_lsclusters - list the PostgreSQL clusters running on this machine',])
endif

pg_lsclusters = executable('pg_lsclusters',
  pg_lsclusters_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_lsclusters

#!/usr/bin/env python3
"""Configure original BusyBox generators and compile native Darwin applets via Bazel."""
import argparse,hashlib,json,os,re,shutil,struct,subprocess,tarfile,tempfile
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'scripts'))
from native_components import run,symbols
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def execute(args,cwd,env=None):
 r=subprocess.run(list(map(str,args)),cwd=cwd,env=env,capture_output=True,text=True)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return r.stdout

def inventory(file,config):
 result=[]
 text=file.read_text().replace('\\\n',' ')
 variables=dict(re.findall(r'^(\w+)\s*:?=\s*(.*)$',text,re.M))
 for line in text.splitlines():
  match=re.match(r'(?:lib|obj)-([y]|\$\(CONFIG_(\w+)\))\s*(?::=|\+=|=)\s*(.*)',line)
  if match and (match[1]=='y' or config.get(match[2])=='y'):
   value=re.sub(r'\$\((\w+)\)',lambda m:variables.get(m[1],''),match[3])
   for item in value.split('#',1)[0].split():
    if item.endswith('.o'):result.append(file.parent/(item[:-2]+'.c'))
 return list(dict.fromkeys(result))

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('root','sdk','config','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args();root=a.root.resolve();original=root/'src/Applications/busybox/1.38.0';manifest=json.loads((original/'UPSTREAM.json').read_text())
 for file,h in manifest['upstream_files_sha256'].items():
  if sha(original/file)!=h:raise ValueError('Modified original BusyBox source: '+file)
 with tempfile.TemporaryDirectory(prefix='.native-busybox-',dir=a.output.parent) as directory:
  work=Path(directory).resolve();source=work/'source';shutil.copytree(original,source);sdk=work/'sdk';os.environ['TMPDIR']=str(work)
  with tarfile.open(a.sdk) as archive:archive.extractall(sdk,filter='data')
  if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('BusyBox SDK mismatch')
  # Original generators; the generated Kbuild files are inventory data only.
  for name,target in (('ps','procps'),('df','coreutils')):
   original_text=(source/target/(name+'.c')).read_text()
   directives='\n'.join(line for line in original_text.splitlines() if line.startswith('//config:') or line.startswith('//applet:') or line.startswith('//kbuild:'))
   usage=f'\n//usage:#define {name}_trivial_usage "'+('[-aAxuw] [aux]' if name=='ps' else '[-hkP] [FILE]...')+f'"\n//usage:#define {name}_full_usage "\\n\\nNative Darwin '+('process snapshot' if name=='ps' else 'filesystem space')+'"\n'
   (source/target/(name+'.c')).write_text(directives+usage+(root/'userland/apps/busybox'/(name+'_darwin.c')).read_text())
  execute(['sh',source/'scripts/gen_build_files.sh',source,source],source)
  bc=source/'miscutils/bc.c'
  bc.write_text(bc.read_text().replace('SSIZE_MAX','((ssize_t)(SIZE_MAX >> 1))'))
  # Keep upstream's Unix account cache; omit its unrelated Linux /proc scanner.
  procps=source/'libbb/procps.c'
  procps.write_text(procps.read_text().split('#define PROCPS_BUFSIZE 1024',1)[0])
  # This upstream helper polls exactly one input descriptor. Darwin pselect
  # preserves its atomic signal-mask transition without a Linux ppoll shim.
  polling=source/'libbb/poll_with_signals.c'
  polling.write_text('#include <sys/select.h>\n'+polling.read_text().replace('n = ppoll(pfd, 1, timeout >= 0 ? &tv : NULL, &orig_mask);', 'fd_set input; FD_ZERO(&input); FD_SET(pfd[0].fd, &input);\n n = pselect(pfd[0].fd + 1, &input, NULL, NULL, timeout >= 0 ? &tv : NULL, &orig_mask);\n pfd[0].revents = n > 0 ? POLLIN : 0;'))
  # Darwin exposes byte swaps through compiler intrinsics and lacks mempcpy.
  platform=source/'include/platform.h'
  platform.write_text(platform.read_text().replace('#if defined(__APPLE__)\n# undef HAVE_STRCHRNUL', '#if defined(__APPLE__)\n# undef HAVE_MEMPCPY\n# undef HAVE_MEMRCHR\n# undef HAVE_STRCHRNUL').replace('__bswap64','__builtin_bswap64').replace('__bswap32','__builtin_bswap32').replace('__bswap16','__builtin_bswap16'))
  kconfig=source/'scripts/kconfig'
  for name in ('zconf.tab.c','lex.zconf.c','zconf.hash.c'):shutil.copyfile(kconfig/(name+'_shipped'),kconfig/name)
  execute(['clang-20','-O2','-DLKC_DIRECT_LINK','-I',kconfig,kconfig/'conf.c',kconfig/'zconf.tab.c','-o',work/'conf'],source)
  execute([work/'conf','-n','Config.in'],source)
  settings=dict(re.findall(r'^CONFIG_(\w+)=(.*)$',a.config.read_text(),re.M))
  lines=(source/'.config').read_text().splitlines()
  lines=[line for line in lines if not any(re.match(r'(?:CONFIG_|# CONFIG_)'+re.escape(key)+r'(?:=| is not set)',line) for key in settings)]
  (source/'.config').write_text('\n'.join(lines)+'\n'+a.config.read_text())
  execute([work/'conf','-s','Config.in'],source)
  config=dict(re.findall(r'^CONFIG_(\w+)=(.*)$',(source/'.config').read_text(),re.M))
  if config.get('PLATFORM_LINUX')=='y':raise ValueError('Linux platform enabled in native BusyBox')
  missing=[key for key,value in settings.items() if value!='n' and config.get(key)!=value]
  if missing:raise ValueError('Requested BusyBox options unavailable: '+', '.join(missing))
  include=source/'include';version=manifest['version'];(include/'bbconfigopts.h').write_text('')
  (include/'bbconfigopts_bz2.h').write_text('')
  (include/'embedded_scripts.h').write_text('#define NUM_SCRIPTS 0\n')
  (include/'libbb_const.h').write_text('')
  (include/'NUM_APPLETS.h').write_text('')
  (include/'config.h').write_text('#include "autoconf.h"\n')
  (include/'bbconfigopts.h').write_text('')
  (include/'version.h').write_text(f'#define BB_VER "{version}"\n#define BB_EXTRA_VERSION "-Darwin"\n#define BB_BT "native Bazel build"\n')
  (include/'libbb.h').write_text('#include "version.h"\n#include <arpa/inet.h>\n#include <limits.h>\n#include <sys/syslimits.h>\n#define st_atim st_atimespec\n#define st_mtim st_mtimespec\n#define st_ctim st_ctimespec\n'+(include/'libbb.h').read_text())
  (include/'bbconfigopts_bz2.h').write_text('')
  for name in ('applet_tables','usage'):
   execute(['clang-20','-O2','-I',include,source/'applets'/(name+'.c'),'-o',work/name],source)
  execute([work/'applet_tables',include/'applet_tables.h',include/'NUM_APPLETS.h'],source)
  shutil.copyfile(work/'usage',source/'applets/usage')
  (source/'applets/usage').chmod(0o755)
  execute(['sh',source/'applets/usage_compressed',include/'usage_compressed.h',source/'applets'],source)
  execute(['sh',source/'scripts/generate_BUFSIZ.sh',include/'common_bufsiz.h'],source)
  resource=run(['clang-20','-print-resource-dir']).strip()
  flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-Os','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections','-nostdinc','-D_DARWIN_C_SOURCE=1','-D_FORTIFY_SOURCE=0','-D_GNU_SOURCE=1','-Werror=implicit-function-declaration','-Wno-availability','-Wno-incompatible-pointer-types','-isystem',resource+'/include','-isystem',sdk/'usr/include','-I',include,'-include',include/'autoconf.h']
  flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
  files=[]
  for recipe in source.rglob('Kbuild'):
   if recipe.is_relative_to(source/'scripts'):continue
   files+=inventory(recipe,config)
  # These unconditional Linux-only helpers have no caller in this profile.
  excluded={'alloc_affinity.c','get_console.c','find_root_device.c','find_pid_by_name.c','find_mount_point.c'}
  files=[f for f in dict.fromkeys(files) if f.name not in excluded]
  # Hardware SHA assembly is optional; use upstream's portable C hashes.
  if config.get('SHA1_SMALL')=='0' or any(config.get(k)=='y' for k in ('SHA1_HWACCEL','SHA256_HWACCEL')):
   raise ValueError('Unexpected hardware hash configuration')
  files=[f for f in files if not f.name.startswith(('hash_sha1_','hash_sha256_'))]
  objects=work/'objects';objects.mkdir();jobs=[(f,objects/f'busybox-{i:03d}.o') for i,f in enumerate(files)]
  def compile_one(job):
   file,obj=job
   if not file.exists():return {'error':'Missing inventory source: '+str(file.relative_to(source))}
   result=subprocess.run(list(map(str,[*flags,'-c',file,'-o',obj])),capture_output=True,text=True)
   if result.returncode:return {'error':str(file.relative_to(source))+':\n'+result.stderr}
   if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign BusyBox object')
   return {'source':str(file.relative_to(source)),'source_sha256':sha(original/file.relative_to(source)),'compiled_source_sha256':sha(file),'member':obj.name,'object_sha256':sha(obj)}
  with ThreadPoolExecutor(max_workers=12) as pool:records=list(pool.map(compile_one,jobs))
  errors=[r['error'] for r in records if 'error' in r]
  if errors:raise ValueError('BusyBox native compilation failed:\n'+'\n'.join(errors))
  run(['llvm-ar-20','--format=darwin','rcsD',a.output,*[obj for _,obj in jobs]])
  exports,imports,weak=symbols(a.output);a.imports.write_text('\n'.join(sorted(imports-exports-{'___dso_handle'}))+'\n')
  # Ask the Mach-O linker which APIs survive actual applet reachability.
  linked=work/'busybox-imports'
  run(['/usr/lib/llvm-20/bin/ld64.lld','-arch',a.arch,'-platform_version','macos','26.5','26.5','-e','_main','-dead_strip','-undefined','dynamic_lookup','-o',linked,a.output])
  _,imports,weak=symbols(linked);a.imports.write_text('\n'.join(sorted(imports))+'\n')
  applets=re.findall(r'^"([^"\\]+)" "\\0"', (include/'applet_tables.h').read_text(),re.M)
  if not {'ls','cp','grep','ps','df','vi'}.issubset(applets):raise ValueError('Missing required native applets')
  a.report.write_text(json.dumps({'architecture':a.arch,'version':version,'configuration':config,'applets':applets,'sources':records,'archive_sha256':sha(a.output),'original_archive_sha256':manifest['archive_sha256'],'private_port_sources':{name:sha(root/'userland/apps/busybox'/name) for name in ('ps_darwin.c','df_darwin.c')},'private_adaptations':{'include/platform.h':sha(platform),'include/libbb.h':sha(include/'libbb.h')},'excluded_uncalled_linux_helpers':sorted(excluded),'runtime_checked':False},indent=2)+'\n')
  print(f'Native BusyBox {a.arch}: {len(records)} Mach-O objects')
if __name__=='__main__':main()

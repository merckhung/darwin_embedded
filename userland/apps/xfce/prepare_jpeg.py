#!/usr/bin/env python3
"""Join original JPEG and JPEG-enabled GdkPixbuf archives into a native stage."""
import argparse,json,tarfile
from pathlib import Path
from build_base import digest
def main():
    p=argparse.ArgumentParser()
    for name in ['jpeg','jpeg-report','pixbuf','pixbuf-report','output','report']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',required=True,choices=['arm64','x86_64']);a=p.parse_args()
    jpeg=json.loads(a.jpeg_report.read_text());pixbuf=json.loads(a.pixbuf_report.read_text())
    if jpeg['architecture']!=a.arch or pixbuf['architecture']!=a.arch or digest(a.jpeg)!=jpeg['bundle_sha256'] or digest(a.pixbuf)!=pixbuf['bundle_sha256']:raise ValueError('native JPEG bundle mismatch')
    libraries={**jpeg['libraries'],**pixbuf['libraries']};files={}
    with tarfile.open(a.pixbuf) as bundle:
        for member in bundle:
            if member.isfile():files[member.name]=bundle.extractfile(member).read()
    import hashlib
    libraries['libgdk_pixbuf-2.0']['archive_sha256']=hashlib.sha256(files['usr/lib/libgdk_pixbuf-2.0.a']).hexdigest()
    with tarfile.open(a.output,'w') as output:
        import io
        for name,data in sorted(files.items()):
            member=tarfile.TarInfo(name);member.size=len(data);member.mode=0o644;member.uid=member.gid=member.mtime=0;output.addfile(member,io.BytesIO(data))
    a.report.write_text(json.dumps({'architecture':a.arch,'libraries':libraries,'bundle_sha256':digest(a.output),'runtime_checked':False,'builtin_loaders':pixbuf['builtin_loaders']},indent=2)+'\n')
if __name__=='__main__':main()

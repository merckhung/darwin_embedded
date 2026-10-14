/* Native Darwin mounted filesystem accounting. SPDX-License-Identifier: GPL-2.0-only */
#include "libbb.h"
#include <sys/mount.h>
static void display(const struct statfs *s,int human)
{
 uint64_t total=s->f_blocks*(uint64_t)s->f_bsize,freebytes=s->f_bfree*(uint64_t)s->f_bsize,avail=s->f_bavail*(uint64_t)s->f_bsize;
 uint64_t used=total-freebytes,den=used+avail;
 if(human)printf("%-20s %9s ",s->f_mntfromname,make_human_readable_str(total,1,0));
 else printf("%-20s %9llu ",s->f_mntfromname,total/1024);
 if(human){printf("%9s ",make_human_readable_str(used,1,0));printf("%9s ",make_human_readable_str(avail,1,0));}
 else printf("%9llu %9llu ",used/1024,avail/1024);
 printf("%3llu%% %s\n",den?(used*100+den-1)/den:0,s->f_mntonname);
}
int df_main(int argc,char **argv) MAIN_EXTERNALLY_VISIBLE;
int df_main(int argc,char **argv)
{
 unsigned opts=getopt32(argv,"hkP");int human=opts&1,result=0;
 puts(human?"Filesystem                Size      Used Available Use% Mounted on":"Filesystem           1K-blocks      Used Available Use% Mounted on");
 if(optind<argc)for(int i=optind;i<argc;i++){struct statfs s;if(statfs(argv[i],&s)){bb_perror_msg("%s",argv[i]);result=1;}else display(&s,human);}
 else {int bytes=getfsstat(NULL,0,MNT_NOWAIT);if(bytes<0)bb_perror_msg_and_die("mount list");int n=bytes+8;struct statfs *s=xmalloc(n*sizeof(*s));n=getfsstat(s,n*sizeof(*s),MNT_NOWAIT);if(n<0)bb_perror_msg_and_die("mount list");for(int i=0;i<n;i++)display(&s[i],human);free(s);}
 return result;
}

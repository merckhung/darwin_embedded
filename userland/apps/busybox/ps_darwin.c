/* Native Darwin process inspection for BusyBox. SPDX-License-Identifier: GPL-2.0-only */
#include "libbb.h"
#include <sys/proc_info.h>
#include <sys/proc_info_private.h>
#include <sys/sysctl.h>

/* The original XNU libsyscall entry point; no Linux /proc parser or ABI. */
extern int __proc_info(int, int, int, uint64_t, void *, int);

static int compare_pid(const void *a, const void *b)
{
 pid_t x=*(const pid_t *)a,y=*(const pid_t *)b;
 return (x>y)-(x<y);
}

int ps_main(int argc,char **argv) MAIN_EXTERNALLY_VISIBLE;
int ps_main(int argc,char **argv)
{
 int all=0,user=0;
 for(int i=1;i<argc;i++) {
  const char *p=argv[i];if(*p=='-')p++;
  for(;*p;p++)switch(*p) {
   case 'a':case 'x':case 'A':case 'e':all=1;break;
   case 'u':user=1;break;case 'w':break;
   default:bb_show_usage();
  }
 }
 int bytes=__proc_info(PROC_INFO_CALL_LISTPIDS,PROC_ALL_PIDS,0,0,NULL,0);
 if(bytes<0)bb_perror_msg_and_die("process list");
 int capacity=bytes+1024;
 pid_t *pids=xmalloc(capacity);
 bytes=__proc_info(PROC_INFO_CALL_LISTPIDS,PROC_ALL_PIDS,0,0,pids,capacity);
 if(bytes<0)bb_perror_msg_and_die("process list");
 int count=bytes/(int)sizeof(pid_t);qsort(pids,count,sizeof(pid_t),compare_pid);
 uint64_t memory=0;size_t size=sizeof(memory);int memkey[]={CTL_HW,HW_MEMSIZE};
 int memory_ok=sysctl(memkey,2,&memory,&size,NULL,0)==0 && memory;
 struct timeval now;gettimeofday(&now,NULL);
 puts(user?"USER       PID PPID  %CPU %MEM      VSZ      RSS STAT     TIME COMMAND":"  PID  PPID USER     STAT     TIME COMMAND");
 for(int i=0;i<count;i++) {
  struct proc_taskallinfo info;
  if(pids[i]<=0||__proc_info(PROC_INFO_CALL_PIDINFO,pids[i],PROC_PIDTASKALLINFO,0,&info,sizeof(info))!=(int)sizeof(info))continue;
  struct proc_bsdinfo *b=&info.pbsd;struct proc_taskinfo *t=&info.ptinfo;
  if(!all && b->pbi_uid!=getuid())continue;
  struct passwd *pw=getpwuid(b->pbi_uid);char uid[24];snprintf(uid,sizeof(uid),"%u",b->pbi_uid);
  const char *owner=pw?pw->pw_name:uid;
  uint64_t ns=t->pti_total_user+t->pti_total_system,seconds=ns/1000000000;
  double elapsed=(double)now.tv_sec-b->pbi_start_tvsec+((double)now.tv_usec-b->pbi_start_tvusec)/1000000;
  char cpu[24],mem[24];
  if(elapsed>0)snprintf(cpu,sizeof(cpu),"%.1f",(double)ns/10000000/elapsed);else strcpy(cpu,"?");
  if(memory_ok)snprintf(mem,sizeof(mem),"%.1f",100.0*t->pti_resident_size/memory);else strcpy(mem,"?");
  const char *status=b->pbi_status==5?"Z":b->pbi_status==4?"T":t->pti_numrunning?"R":"S";
  char command[8192];size=sizeof(command);int key[]={CTL_KERN,KERN_PROCARGS2,pids[i]};
  const char *name=b->pbi_name[0]?b->pbi_name:b->pbi_comm;
  if(sysctl(key,3,command,&size,NULL,0)==0 && size>sizeof(int)) {
   int nargs;memcpy(&nargs,command,sizeof(nargs));
   char *start=command+sizeof(int),*end=command+size;
   while(start<end&&*start)start++;while(start<end&&!*start)start++;
   if(start<end&&nargs>0) {
    char *p=start;for(int arg=0;arg<nargs&&p<end;arg++) {while(p<end&&*p)p++;if(p<end){*p=(arg+1<nargs)?' ':'\0';p++;}}
    end[-1]='\0';name=start;
   }
  }
  if(user)printf("%-8s %5u %4u %5s %4s %8llu %8llu %-4s %3llu:%02llu %s\n",owner,b->pbi_pid,b->pbi_ppid,cpu,mem,t->pti_virtual_size/1024,t->pti_resident_size/1024,status,seconds/60,seconds%60,name);
  else printf("%5u %5u %-8s %-4s %3llu:%02llu %s\n",b->pbi_pid,b->pbi_ppid,owner,status,seconds/60,seconds%60,name);
 }
 free(pids);return fflush(stdout)==EOF;
}

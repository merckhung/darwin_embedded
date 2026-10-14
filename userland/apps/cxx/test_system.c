/* Exercise actual LLVM TI arithmetic and original Darwin unlinkat in a guest. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c,label) do { if (!(c)) {fprintf(stderr,"Native C++ system failure: %s errno=%d\n",label,errno);return 1;} } while(0)
typedef unsigned __int128 uint128;
int main(int argc,char **argv)
{
    CHECK(argc==2 && strlen(argv[1])==48,"nonce");
    volatile uint128 a=((uint128)0xfedcba9876543210ULL<<64)|0x0123456789abcdefULL;
    volatile uint128 b=97;
    uint128 q=a/b,r=a-q*b;
    CHECK(q==(((uint128)0x02a0a04630b99c3aULL<<64)|0x3a12d67d1bcfa079ULL) && r==0x16,"original-unsigned-int128-division");
    volatile __int128 sa=-((__int128)1<<126);sa=sa*2+345;
    volatile __int128 sb=((__int128)65537<<64)|3;
    __int128 sq=sa/sb,sr=sa-sq*sb;
    CHECK((uint128)sq==(((uint128)0xffffffffffffffffULL<<64)|0xffff80007fff8001ULL) &&
          (uint128)sr==(((uint128)0xffffffffffff7fffULL<<64)|0x00017ffe80018156ULL),"original-signed-int128-division");
    char path[128],name[96];snprintf(name,sizeof(name),"native-cxx-%s",argv[1]);snprintf(path,sizeof(path),"/tmp/%s",name);
    int file=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(file>=0 && !close(file),"create-real-file");
    int directory=open("/tmp",O_RDONLY);CHECK(directory>=0,"open-native-directory");
    CHECK(!unlinkat(directory,name,0),"original-relative-unlinkat");
    struct stat status;errno=0;CHECK(stat(path,&status)==-1 && errno==ENOENT,"file-really-removed");
    errno=0;CHECK(unlinkat(directory,name,0)==-1 && errno==ENOENT,"actual-missing-file-error");
    CHECK(!mkdir(path,0700) && !unlinkat(directory,name,AT_REMOVEDIR),"native-directory-removal-flag");
    errno=0;CHECK(stat(path,&status)==-1 && errno==ENOENT,"directory-really-removed");
    CHECK(!close(directory),"close-native-directory");
    struct timespec start,end;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&start) && !usleep(100000) &&
          !clock_gettime(CLOCK_MONOTONIC,&end),"original-monotonic-clock");
    long long elapsed=(end.tv_sec-start.tv_sec)*1000000000LL+end.tv_nsec-start.tv_nsec;
    CHECK(elapsed>=50000000 && elapsed<5000000000LL,"actual-commpage-boottime-clock-progress");
    struct utsname identity;CHECK(!uname(&identity),"uname");
    printf("NATIVE_CXX_SYSTEM_OK:%s:%s:%s\n",argv[1],identity.release,identity.machine);return 0;
}

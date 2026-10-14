/* Actual Darwin cloning PTY, terminal ioctls and an interactive Mach-O shell. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr,"Native PTY failure: %s errno=%d\n",label,errno); return 1; } } while (0)
int main(int argc, char **argv)
{
    CHECK(argc==2 && strlen(argv[1])==48,"nonce");
    const char *nonce=argv[1];struct utsname identity;
    CHECK(!uname(&identity),"uname");
    int master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK);
    CHECK(master>=0,"real-devfs-ptmx");
    CHECK(!grantpt(master) && !unlockpt(master),"native-grant-and-unlock");
    char name[128];CHECK(!ptsname_r(master,name,sizeof(name)),"actual-slave-name");
    struct stat status;CHECK(!stat(name,&status) && S_ISCHR(status.st_mode),"real-slave-character-device");
    char small[1];errno=0;
    CHECK(ptsname_r(master,small,sizeof(small))==-1 && errno==ERANGE,"slave-name-buffer-error");
    int slave=open(name,O_RDWR|O_NOCTTY);CHECK(slave>=0,"open-slave");
    struct termios attributes;CHECK(!tcgetattr(slave,&attributes),"native-termios");
    attributes.c_lflag|=ICANON;attributes.c_lflag&=~ECHO;
    CHECK(!tcsetattr(slave,TCSANOW,&attributes),"canonical-input-without-echo");
    struct winsize dimensions={.ws_row=37,.ws_col=103},actual;
    CHECK(!ioctl(master,TIOCSWINSZ,&dimensions) && !ioctl(slave,TIOCGWINSZ,&actual) &&
          actual.ws_row==37 && actual.ws_col==103,"real-terminal-dimensions");
    pid_t child=fork();CHECK(child>=0,"fork-native-shell");
    if (!child) {
        close(master);
        if (setsid()<0 || ioctl(slave,TIOCSCTTY,0)<0 ||
            dup2(slave,0)<0 || dup2(slave,1)<0 || dup2(slave,2)<0)_exit(120);
        if (slave>2)close(slave);
        execl("/bin/sh","sh","-i",(char *)NULL);_exit(121);
    }
    char command[160],expected[96],output[4096];size_t used=0;
    int length=snprintf(command,sizeof(command),"printf '\\nNATIVE_PTY_SHELL:%s\\n'; exit 0\n",nonce);
    CHECK(length>0 && (size_t)length<sizeof(command) && write(master,command,(size_t)length)==length,"terminal-input");
    snprintf(expected,sizeof(expected),"\nNATIVE_PTY_SHELL:%s\r\n",nonce);
    int found=0;output[0]=0;
    for (unsigned retry=0;retry<300 && !found;++retry) {
        struct pollfd descriptor={.fd=master,.events=POLLIN};int ready=poll(&descriptor,1,200);
        if (ready<0 && errno==EINTR)continue;
        CHECK(ready>=0,"master-poll");
        if (ready>0) {
            ssize_t count=read(master,output+used,sizeof(output)-used-1);
            if (count<0 && (errno==EINTR || errno==EAGAIN))continue;
            CHECK(count>0,"real-shell-terminal-output");
            used+=(size_t)count;output[used]=0;found=strstr(output,expected)!=NULL;
            CHECK(found || used<sizeof(output)-1,"terminal-output-bounds");
        }
    }
    if (!found) {kill(child,SIGKILL);fprintf(stderr,"Native PTY received: %s\n",output);}
    CHECK(found,"actual-shell-output-nonce");
    int exit_status;pid_t waited;
    do {waited=waitpid(child,&exit_status,0);} while (waited<0 && errno==EINTR);
    CHECK(waited==child && WIFEXITED(exit_status) && WEXITSTATUS(exit_status)==0,"native-shell-clean-exit");
    CHECK(!close(slave) && !close(master),"close-pty");
    printf("NATIVE_PTY_OK:%s:%s:%s\n",nonce,identity.release,identity.machine);
    return 0;
}

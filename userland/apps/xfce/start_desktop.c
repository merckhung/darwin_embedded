/* Native embedded launcher for the original Xorg and Xfce session manager. */
#include <X11/Xlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pid_t server, session;
static volatile sig_atomic_t stopping;
static void stop(int signal_number) { stopping=signal_number; }
static int status_code(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128+WTERMSIG(status);
    return 1;
}
static void terminate(pid_t child)
{
    if (child<=0) return;
    if (kill(child,SIGTERM) && errno!=ESRCH) perror("Native desktop: kill child");
    int status;
    while (waitpid(child,&status,0)<0 && errno==EINTR) {}
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--session")) {
        const char *address=getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !*address) {fprintf(stderr,"Native desktop: session bus address missing\n");return 1;}
        umask(077);
        FILE *file=fopen("/var/run/xfce-session.bus","w");
        if (!file) {perror("Native desktop: save session bus address");return 1;}
        if (fprintf(file,"%s\n",address)<0 || fclose(file)) {perror("Native desktop: write session bus address");return 1;}
        execl("/usr/bin/xfce4-session","xfce4-session",(char *)NULL);
        perror("Native desktop: exec Xfce session");return 127;
    }
    if (argc!=1) {fprintf(stderr,"Native desktop: invalid arguments\n");return 1;}
    struct sigaction action={0}; action.sa_handler=stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL) ||
        setenv("DISPLAY",":0",1) || setenv("XDG_CURRENT_DESKTOP","XFCE",1) ||
        setenv("XDG_SESSION_TYPE","x11",1) || setenv("DESKTOP_SESSION","xfce",1) ||
        setenv("TERM","xterm-256color",1)) {
        perror("Native desktop: environment/signals"); return 1;
    }
    server=fork();
    if (server<0) {perror("Native desktop: fork Xorg"); return 1;}
    if (!server) {
        execl("/usr/bin/Xorg","Xorg",":0","-nolisten","tcp","-noreset",(char *)NULL);
        perror("Native desktop: exec Xorg"); _exit(127);
    }
    Display *display=NULL;
    for (unsigned retry=0;retry<1200 && !display && !stopping;++retry) {
        int status;pid_t child=waitpid(server,&status,WNOHANG);
        if (child==server) {
            fprintf(stderr,"Native desktop: Xorg exited before display readiness, status=%d\n",status_code(status));
            return status_code(status)?status_code(status):1;
        }
        if (child<0 && errno!=EINTR) {perror("Native desktop: wait Xorg");terminate(server);return 1;}
        display=XOpenDisplay(":0");if (!display) usleep(50000);
    }
    if (!display || stopping) {fprintf(stderr,"Native desktop: display startup interrupted or timed out\n");terminate(server);return 1;}
    XCloseDisplay(display);
    session=fork();
    if (session<0) {perror("Native desktop: fork session");terminate(server);return 1;}
    if (!session) {
        execl("/usr/bin/dbus-run-session","dbus-run-session","--","/bin/native-xfce-start","--session",(char *)NULL);
        perror("Native desktop: exec session bus");_exit(127);
    }
    printf("Native desktop: original Xorg pid=%ld, session bus pid=%ld\n",(long)server,(long)session);fflush(stdout);
    int result=1;
    while (!stopping) {
        int status;pid_t child=waitpid(-1,&status,0);
        if (child<0) {if(errno==EINTR)continue;perror("Native desktop: wait children");break;}
        if (child==session) {session=0;result=status_code(status);break;}
        if (child==server) {server=0;result=status_code(status);if (!result)result=1;break;}
    }
    terminate(session);terminate(server);
    printf("Native desktop: session ended, status=%d\n",result);return result;
}

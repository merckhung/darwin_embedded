/* Actual native Xorg child, X11 window rendering and hardware-input probe. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define CHECK(c,label) do { if (!(c)) { fprintf(stderr,"NATIVE_XORG_FAIL:%s\n",label); return 1; } } while (0)

int main(int argc,char **argv)
{
    CHECK(argc==2 && strlen(argv[1])==48,"nonce");
    struct utsname identity;CHECK(!uname(&identity),"uname");
    pid_t server=fork();CHECK(server>=0,"fork-server");
    if (!server) {
        execl("/usr/bin/Xorg","Xorg",":0","-nolisten","tcp","-noreset",(char *)NULL);
        _exit(127);
    }
    Display *display=NULL;
    for (unsigned retry=0;retry<1200 && !display;++retry) {
        int status;
        if (waitpid(server,&status,WNOHANG)==server) {
            fprintf(stderr,"NATIVE_XORG_SERVER_EXIT:%d\n",status);return 1;
        }
        display=XOpenDisplay(":0");
        if (!display) usleep(50000);
    }
    CHECK(display,"open-real-display");
    int screen=DefaultScreen(display);Window root=RootWindow(display,screen);
    unsigned width=DisplayWidth(display,screen),height=DisplayHeight(display,screen);
    CHECK(width>=640 && height>=480,"scanout-geometry");
    unsigned left=width/4,top=height/4,w=width/2,h=height/2;
    Window window=XCreateSimpleWindow(display,root,left,top,w,h,0,0,0);
    CHECK(window,"create-window");
    XSelectInput(display,window,ExposureMask|KeyPressMask|ButtonPressMask|StructureNotifyMask);
    char title[96];snprintf(title,sizeof(title),"Native Xorg %s",argv[1]);XStoreName(display,window,title);
    Atom pid_atom=XInternAtom(display,"_NET_WM_PID",False),cardinal=XInternAtom(display,"CARDINAL",False);
    unsigned long pid=(unsigned long)getpid();XChangeProperty(display,window,pid_atom,cardinal,32,PropModeReplace,(unsigned char *)&pid,1);
    XMapWindow(display,window);XSync(display,False);
    XSetInputFocus(display,window,RevertToParent,CurrentTime);
    XChangePointerControl(display,True,True,1,1,0);XSync(display,False);
    GC gc=XCreateGC(display,window,0,NULL);CHECK(gc,"create-gc");
    static const unsigned short colors[4][3]={{65535,0,0},{0,65535,0},{0,0,65535},{65535,65535,65535}};
    unsigned long pixels[4];
    for (unsigned i=0;i<4;++i) {
        XColor color={.red=colors[i][0],.green=colors[i][1],.blue=colors[i][2],.flags=DoRed|DoGreen|DoBlue};
        CHECK(XAllocColor(display,DefaultColormap(display,screen),&color),"allocate-real-color");
        pixels[i]=color.pixel;XSetForeground(display,gc,color.pixel);
        unsigned x=i*w/4,next=(i+1)*w/4;XFillRectangle(display,window,gc,x,0,next-x,h);
    }
    XSync(display,False);
    XImage *image=XGetImage(display,window,0,0,w,h,AllPlanes,ZPixmap);CHECK(image,"read-window-pixels");
    for (unsigned i=0;i<4;++i) CHECK(XGetPixel(image,(2*i+1)*w/8,h/2)==pixels[i],"server-render-readback");
    XDestroyImage(image);
    Window child,returned_root;int rx,ry,wx,wy;unsigned mask;
    CHECK(XQueryPointer(display,root,&returned_root,&child,&rx,&ry,&wx,&wy,&mask),"real-pointer-state");
    printf("NATIVE_XORG_READY:%s:%s:%s:%u:%u:%lu:%ld:%ld:%d:%d\n",argv[1],identity.release,identity.machine,
        width,height,window,(long)server,(long)getpid(),rx,ry);CHECK(!fflush(stdout),"flush-ready");
    char acknowledgement;
    CHECK(read(STDIN_FILENO,&acknowledgement,1)==1 && acknowledgement=='\n',"screenshot-ack");
    Bool key=False,button=False;
    for (unsigned retry=0;retry<1200 && !(key && button);++retry) {
        while (XPending(display)) {
            XEvent event;XNextEvent(display,&event);
            if (event.type==KeyPress) {
                char text[8];KeySym symbol;
                int length=XLookupString(&event.xkey,text,sizeof(text),&symbol,NULL);
                CHECK(length==1 && text[0]=='a' && event.xkey.keycode==12 && !event.xkey.send_event,"native-hardware-key");
                key=True;printf("NATIVE_XORG_KEY:%s:%u:%lu\n",argv[1],event.xkey.keycode,symbol);fflush(stdout);
            } else if (event.type==ButtonPress) {
                CHECK(event.xbutton.button==1 && !event.xbutton.send_event && event.xbutton.x>=0 && event.xbutton.x<(int)w &&
                    event.xbutton.y>=0 && event.xbutton.y<(int)h,"native-hardware-button");
                button=True;printf("NATIVE_XORG_BUTTON:%s:%u:%d:%d\n",argv[1],event.xbutton.button,event.xbutton.x,event.xbutton.y);fflush(stdout);
            }
        }
        if (!(key && button)) usleep(50000);
    }
    CHECK(key && button,"hardware-input-timeout");
    XFreeGC(display,gc);XDestroyWindow(display,window);XCloseDisplay(display);
    CHECK(kill(server,SIGTERM)==0,"stop-server");
    int status;pid_t waited;
    do {waited=waitpid(server,&status,0);} while (waited<0 && errno==EINTR);
    CHECK(waited==server && WIFEXITED(status) && WEXITSTATUS(status)==0,"server-clean-exit");
    printf("NATIVE_XORG_OK:%s:%s:%s\n",argv[1],identity.release,identity.machine);
    return 0;
}

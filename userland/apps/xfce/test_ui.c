/* Actual original Xfce/GTK window, Cairo rendering and hardware input. */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <libxfce4ui/libxfce4ui.h>
#include <xfconf/xfconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <servers/bootstrap.h>

#define CHECK(c,label) do { if (!(c)) { fprintf(stderr,"NATIVE_XFCE_UI_FAIL:%s\n",label); return 1; } } while (0)
static const char *nonce;
static gboolean drawn, key_seen, button_seen, response_seen;

static gboolean draw(GtkWidget *widget,cairo_t *context,gpointer unused)
{
    (void)unused;
    int width=gtk_widget_get_allocated_width(widget),height=gtk_widget_get_allocated_height(widget);
    static const double colors[4][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    for (unsigned i=0;i<4;++i) {
        int left=i*width/4,right=(i+1)*width/4;
        cairo_set_source_rgb(context,colors[i][0],colors[i][1],colors[i][2]);
        cairo_rectangle(context,left,0,right-left,height);cairo_fill(context);
    }
    drawn=TRUE;return TRUE;
}
static gboolean key(GtkWidget *widget,GdkEventKey *event,gpointer unused)
{
    (void)widget;(void)unused;
    if (event->send_event || event->hardware_keycode!=12 || event->keyval!=GDK_KEY_a) {
        fprintf(stderr,"NATIVE_XFCE_UI_FAIL:hardware-key\n");exit(1);
    }
    key_seen=TRUE;printf("NATIVE_XFCE_UI_KEY:%s:%u:%u\n",nonce,event->hardware_keycode,event->keyval);fflush(stdout);return TRUE;
}
static gboolean button(GtkWidget *widget,GdkEventButton *event,gpointer unused)
{
    (void)widget;(void)unused;
    if (event->send_event || event->button!=1) {fprintf(stderr,"NATIVE_XFCE_UI_FAIL:hardware-button\n");exit(1);}
    button_seen=TRUE;printf("NATIVE_XFCE_UI_BUTTON:%s:1\n",nonce);fflush(stdout);return FALSE;
}
static void response(GtkDialog *dialog,gint id,gpointer unused)
{
    (void)dialog;(void)unused;
    if (id!=GTK_RESPONSE_OK || !button_seen) {fprintf(stderr,"NATIVE_XFCE_UI_FAIL:real-dialog-response\n");exit(1);}
    response_seen=TRUE;printf("NATIVE_XFCE_UI_RESPONSE:%s:%d\n",nonce,id);fflush(stdout);
}

static int daemon_check(void)
{
    mach_port_t parent=MACH_PORT_NULL;
    CHECK(bootstrap_parent(MACH_PORT_NULL,&parent)==MACH_SEND_INVALID_DEST,"real-bootstrap-rpc-invalid-destination");
    int descriptors[2];CHECK(!pipe(descriptors),"daemon-pipe");
    pid_t runner=fork();CHECK(runner>=0,"fork-daemon-runner");
    if (!runner) {
        close(descriptors[0]);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (daemon(0,1))_exit(1);
#pragma clang diagnostic pop
        for (unsigned retry=0;retry<200 && getppid()!=1;++retry)usleep(1000);
        char cwd[16];pid_t result[3]={getpid(),getppid(),getsid(0)};
        if (!getcwd(cwd,sizeof(cwd)) || strcmp(cwd,"/") || result[0]<=1 || result[1]!=1 || result[2]!=result[0] ||
            write(descriptors[1],result,sizeof(result))!=sizeof(result))_exit(2);
        close(descriptors[1]);_exit(0);
    }
    close(descriptors[1]);pid_t result[3];size_t used=0;
    while (used<sizeof(result)) {
        ssize_t count=read(descriptors[0],(char *)result+used,sizeof(result)-used);
        if (count<0 && errno==EINTR)continue;
        CHECK(count>0,"real-daemon-result");used+=(size_t)count;
    }
    close(descriptors[0]);int status;
    CHECK(waitpid(runner,&status,0)==runner && WIFEXITED(status) && !WEXITSTATUS(status),"daemon-parent-clean-exit");
    CHECK(result[0]!=runner && result[0]!=getpid() && result[1]==1 && result[2]==result[0],"genuine-daemon-session");
    printf("NATIVE_XFCE_UI_DAEMON:%s:%ld:%ld:%ld\n",nonce,(long)result[0],(long)result[1],(long)result[2]);fflush(stdout);
    return 0;
}

static unsigned long property_number(Display *display,Window window,const char *name,Atom type)
{
    Atom actual;int format;unsigned long count,remaining;unsigned char *data=NULL;unsigned long value=0;
    if (XGetWindowProperty(display,window,XInternAtom(display,name,False),0,1,False,type,&actual,&format,&count,&remaining,&data)==Success &&
        actual==type && format==32 && count==1 && !remaining)value=*(unsigned long *)data;
    if (data)XFree(data);return value;
}
static guint32 service_pid(GDBusConnection *bus,const char *name)
{
    GError *error=NULL;guint32 pid=0;
    GVariant *reply=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",
        "GetConnectionUnixProcessID",g_variant_new("(s)",name),G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,10000,NULL,&error);
    if (reply) {g_variant_get(reply,"(u)",&pid);g_variant_unref(reply);}g_clear_error(&error);return pid;
}
static gboolean desktop_windows(Display *display,guint32 desktop_pid,guint32 panel_pid,Window *desktop,Window panels[2])
{
    Atom actual;int format;unsigned long count,remaining;unsigned char *data=NULL;unsigned panel_count=0;*desktop=None;panels[0]=panels[1]=None;
    if (XGetWindowProperty(display,DefaultRootWindow(display),XInternAtom(display,"_NET_CLIENT_LIST",False),0,1024,False,XA_WINDOW,&actual,&format,&count,&remaining,&data)!=Success ||
        actual!=XA_WINDOW || format!=32 || remaining) {if(data)XFree(data);return FALSE;}
    for (unsigned long i=0;i<count;++i) {
        Window window=((Window *)data)[i];unsigned long pid=property_number(display,window,"_NET_WM_PID",XA_CARDINAL);
        unsigned long type=property_number(display,window,"_NET_WM_WINDOW_TYPE",XA_ATOM);XWindowAttributes attributes;
        if (!XGetWindowAttributes(display,window,&attributes) || attributes.map_state!=IsViewable)continue;
        if (pid==desktop_pid && type==XInternAtom(display,"_NET_WM_WINDOW_TYPE_DESKTOP",False))*desktop=window;
        if (pid==panel_pid && type==XInternAtom(display,"_NET_WM_WINDOW_TYPE_DOCK",False) && panel_count<2)panels[panel_count++]=window;
    }
    if(data)XFree(data);
    if (!*desktop || panel_count!=2)return FALSE;
    Window child;int x[2],y[2];XWindowAttributes attributes[2];
    for (unsigned i=0;i<2;++i) {
        if (!XGetWindowAttributes(display,panels[i],&attributes[i]) ||
            !XTranslateCoordinates(display,panels[i],DefaultRootWindow(display),0,0,&x[i],&y[i],&child))return FALSE;
    }
    if (y[0]>y[1]) {Window temp=panels[0];panels[0]=panels[1];panels[1]=temp;}
    int top=y[0]<y[1]?0:1,bottom=1-top,height=DisplayHeight(display,DefaultScreen(display));
    return y[top]<=64 && y[bottom]>=height-128 && y[bottom]+attributes[bottom].height>=height-4;
}

int main(int argc,char **argv)
{
    CHECK((argc==2 || (argc==3 && (!strcmp(argv[2],"--wm") || !strcmp(argv[2],"--desktop") || !strcmp(argv[2],"--jpeg") || !strcmp(argv[2],"--clock")))) && strlen(argv[1])==48,"nonce");nonce=argv[1];
    if (argc==3 && !strcmp(argv[2],"--clock")) {
        struct timespec start,end;char input[64];
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&start),"native-clock-start");
        printf("NATIVE_XFCE_CLOCK_READY:%s\n",nonce);fflush(stdout);
        CHECK(fgets(input,sizeof(input),stdin) && !strncmp(input,nonce,48),"host-clock-challenge");
        CHECK(!clock_gettime(CLOCK_MONOTONIC,&end),"native-clock-end");
        long long elapsed=(long long)(end.tv_sec-start.tv_sec)*1000000000LL+end.tv_nsec-start.tv_nsec;
        CHECK(elapsed>0,"monotonic-clock");
        printf("NATIVE_XFCE_CLOCK_ELAPSED:%s:%lld\n",nonce,elapsed);return 0;
    }
    gboolean with_jpeg=argc==3 && !strcmp(argv[2],"--jpeg");
    gboolean with_wm=argc==3,with_desktop=with_jpeg || (argc==3 && !strcmp(argv[2],"--desktop"));pid_t manager=0,session=0;Window manager_window=0;XfconfChannel *manager_configuration=NULL;
    if (with_jpeg) {
        GError *decode_error=NULL;
        GdkPixbuf *image=gdk_pixbuf_new_from_file("/usr/share/backgrounds/xfce/xfce-blue.jpg",&decode_error);
        if (!image && decode_error)fprintf(stderr,"Native JPEG: %s\n",decode_error->message);
        CHECK(image && gdk_pixbuf_get_width(image)==2880 && gdk_pixbuf_get_height(image)==1800 &&
            gdk_pixbuf_get_n_channels(image)==3 && gdk_pixbuf_get_bits_per_sample(image)==8,"actual-original-jpeg-decode");
        const guchar *pixels=gdk_pixbuf_read_pixels(image);int stride=gdk_pixbuf_get_rowstride(image);
        printf("NATIVE_XFCE_JPEG:%s:2880:1800",nonce);
        for (unsigned i=1;i<=3;++i) {
            const guchar *pixel=pixels+(450*i)*stride+(720*i)*3;
            printf(":%u:%u:%u",pixel[0],pixel[1],pixel[2]);
        }
        printf("\n");fflush(stdout);g_object_unref(image);g_clear_error(&decode_error);
    }
    struct utsname identity;CHECK(!uname(&identity),"uname");
    CHECK(!daemon_check(),"native-daemon-and-bootstrap");
    pid_t server=fork();CHECK(server>=0,"fork-server");
    if (!server) {execl("/usr/bin/Xorg","Xorg",":0","-nolisten","tcp","-noreset",(char *)NULL);_exit(127);}
    Display *display=NULL;
    for (unsigned retry=0;retry<1200 && !display;++retry) {
        int status;
        CHECK(waitpid(server,&status,WNOHANG)!=server,"server-exited");
        display=XOpenDisplay(":0");if (!display)usleep(50000);
    }
    CHECK(display,"open-real-display");CHECK(!setenv("DISPLAY",":0",1),"display-environment");
    GError *error=NULL;GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);CHECK(bus,"real-session-bus");
    GVariant *address=g_dbus_connection_call_sync(bus,"org.a11y.Bus","/org/a11y/bus","org.a11y.Bus","GetAddress",NULL,G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,120000,NULL,&error);
    CHECK(address,"real-accessibility-bus-startup");const char *bus_address;g_variant_get(address,"(&s)",&bus_address);
    CHECK(bus_address && *bus_address,"actual-accessibility-bus-address");g_variant_unref(address);
    if (with_wm) {
        GVariant *activation=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","StartServiceByName",
            g_variant_new("(su)","org.xfce.Xfconf",0u),G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,120000,NULL,&error);
        CHECK(activation,"real-xfconf-service-startup");guint32 started;g_variant_get(activation,"(u)",&started);
        CHECK(started==1 || started==2,"real-xfconf-activation-reply");g_variant_unref(activation);
        CHECK(xfconf_init(&error),"real-xfconf-client-ready");manager_configuration=xfconf_channel_new("xfwm4");
        CHECK(manager_configuration && xfconf_channel_set_string(manager_configuration,"/Native/verification",nonce),"real-xfconf-property-roundtrip");
        if (with_desktop) {
            XfconfChannel *configuration=xfconf_channel_new("xfce4-session");CHECK(configuration,"actual-session-configuration");
            CHECK(xfconf_channel_set_int(configuration,"/sessions/Failsafe/Count",3),"session-program-count");
            const char *commands[3]={"xfwm4","xfce4-panel","xfdesktop"};const int priorities[3]={15,25,35};
            for (unsigned i=0;i<3;++i) {
                char property[80];snprintf(property,sizeof(property),"/sessions/Failsafe/Client%u_Command",i);
                const char *command[2]={commands[i],NULL};CHECK(xfconf_channel_set_string_list(configuration,property,command),"real-session-program");
                snprintf(property,sizeof(property),"/sessions/Failsafe/Client%u_Priority",i);CHECK(xfconf_channel_set_int(configuration,property,priorities[i]),"session-startup-order");
            }
            g_object_unref(configuration);
            XfconfChannel *panel=xfconf_channel_new("xfce4-panel");CHECK(panel,"actual-panel-configuration");
            CHECK(xfconf_channel_set_uint(panel,"/panels/panel-2/autohide-behavior",0),"visible-bottom-panel");g_object_unref(panel);
            session=fork();CHECK(session>=0,"fork-real-session-manager");
            if (!session) {execl("/usr/bin/xfce4-session","xfce4-session",(char *)NULL);_exit(127);}
        } else {
            manager=fork();CHECK(manager>=0,"fork-real-window-manager");
            if (!manager) {execl("/usr/bin/xfwm4","xfwm4","--sm-client-disable",(char *)NULL);_exit(127);}
        }
        for (unsigned retry=0;retry<6000 && !manager_window;++retry) {
            int status;pid_t child=with_desktop?session:manager;CHECK(waitpid(child,&status,WNOHANG)!=child,"window-manager-or-session-exited");
            manager_window=property_number(display,DefaultRootWindow(display),"_NET_SUPPORTING_WM_CHECK",XA_WINDOW);
            if (retry%200==0) {
                GVariant *reply=g_dbus_connection_call_sync(bus,"org.xfce.Xfconf","/org/xfce/Xfconf","org.xfce.Xfconf","GetProperty",
                    g_variant_new("(ss)","xfwm4","/Native/verification"),G_VARIANT_TYPE("(v)"),G_DBUS_CALL_FLAGS_NONE,120000,NULL,&error);
                CHECK(reply,"configuration-alive-during-manager-startup");GVariant *saved;
                g_variant_get(reply,"(v)",&saved);CHECK(g_variant_is_of_type(saved,G_VARIANT_TYPE_STRING) && !strcmp(g_variant_get_string(saved,NULL),nonce),"real-configuration-heartbeat-value");
                g_variant_unref(saved);g_variant_unref(reply);
            }
            if (!manager_window)usleep(50000);
        }
        CHECK(manager_window && property_number(display,manager_window,"_NET_SUPPORTING_WM_CHECK",XA_WINDOW)==manager_window,"real-manager-check-window");
        if (with_desktop)manager=(pid_t)property_number(display,manager_window,"_NET_WM_PID",XA_CARDINAL);
        CHECK(property_number(display,manager_window,"_NET_WM_PID",XA_CARDINAL)==(unsigned long)manager,"real-manager-pid-property");
        printf("NATIVE_XFCE_WM_COMPOSITOR:%s:%lu:%lu:%lu\n",nonce,manager_window,
            XGetSelectionOwner(display,XInternAtom(display,"_NET_WM_CM_S0",False)),
            XGetSelectionOwner(display,XInternAtom(display,"COMPOSITING_MANAGER",False)));fflush(stdout);
        if (with_desktop) {
            CHECK(manager>1 && manager!=session,"independent-session-and-window-manager");
            Window desktop=None,panels[2];guint32 desktop_pid=0,panel_pid=0;
            XfconfChannel *visible_panels=xfconf_channel_new("xfce4-panel");CHECK(visible_panels,"actual-panel-settings-channel");
            for (unsigned retry=0;retry<2400;++retry) {
                int status;CHECK(waitpid(session,&status,WNOHANG)!=session,"real-session-died");
                if (!desktop_pid)desktop_pid=service_pid(bus,"org.xfce.xfdesktop");
                if (!panel_pid)panel_pid=service_pid(bus,"org.xfce.Panel");
                if (panel_pid>1 && retry%20==0)
                    CHECK(xfconf_channel_set_uint(visible_panels,"/panels/panel-2/autohide-behavior",0),"actual-bottom-panel-visibility");
                if (desktop_pid>1 && panel_pid>1 && desktop_windows(display,desktop_pid,panel_pid,&desktop,panels))break;
                usleep(50000);
            }
            CHECK(desktop && panels[0] && panels[1] && desktop_pid!=panel_pid && desktop_pid!=(guint32)session && panel_pid!=(guint32)session,"actual-desktop-and-two-panels");
            CHECK(xfconf_channel_get_uint(visible_panels,"/panels/panel-2/autohide-behavior",99)==0,"real-bottom-panel-autohide-setting");g_object_unref(visible_panels);
            CHECK(service_pid(bus,"org.xfce.SessionManager")== (guint32)session,"real-session-service-pid");
            GVariant *clients=g_dbus_connection_call_sync(bus,"org.xfce.SessionManager","/org/xfce/SessionManager","org.xfce.Session.Manager","ListClients",NULL,G_VARIANT_TYPE("(ao)"),G_DBUS_CALL_FLAGS_NONE,120000,NULL,&error);
            CHECK(clients,"real-session-client-list");GVariant *paths=g_variant_get_child_value(clients,0);gsize registered=g_variant_n_children(paths);
            CHECK(registered>=3,"three-real-session-protocol-clients");g_variant_unref(paths);g_variant_unref(clients);
            printf("NATIVE_XFCE_DESKTOP_READY:%s:%ld:%u:%u:%lu:%lu:%lu:%lu\n",nonce,(long)session,desktop_pid,panel_pid,desktop,panels[0],panels[1],(unsigned long)registered);fflush(stdout);
        }
    }
    if (!with_desktop)g_object_unref(bus);
    CHECK(gtk_init_check(NULL,NULL),"real-gtk-init");
    CHECK(!libxfce4ui_check_version(4,20,2),"real-ui-version");
    int screen=DefaultScreen(display),width=DisplayWidth(display,screen),height=DisplayHeight(display,screen);
    char title[96];snprintf(title,sizeof(title),"Native Xfce %s",nonce);
    GtkWidget *dialog=xfce_titled_dialog_new_with_mixed_buttons(title,NULL,0,"","Native response",GTK_RESPONSE_OK,NULL);
    CHECK(dialog && XFCE_IS_TITLED_DIALOG(dialog),"original-xfce-dialog");
    xfce_titled_dialog_set_subtitle(XFCE_TITLED_DIALOG(dialog),"Mach-O on Darwin 25.5.0");
    CHECK(!strcmp(xfce_titled_dialog_get_subtitle(XFCE_TITLED_DIALOG(dialog)),"Mach-O on Darwin 25.5.0"),"real-subtitle");
    gtk_window_move(GTK_WINDOW(dialog),width/4,height/4);gtk_window_set_default_size(GTK_WINDOW(dialog),width/2,height/2);
    GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label=gtk_label_new("Original Xfce UI / GTK3 / X11");gtk_container_add(GTK_CONTAINER(content),label);
    GtkWidget *area=gtk_drawing_area_new();gtk_widget_set_size_request(area,400,180);gtk_widget_set_hexpand(area,TRUE);gtk_widget_set_vexpand(area,TRUE);
    gtk_container_add(GTK_CONTAINER(content),area);g_signal_connect(area,"draw",G_CALLBACK(draw),NULL);
    GtkWidget *action=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_OK);CHECK(action,"real-response-button");
    g_signal_connect(dialog,"key-press-event",G_CALLBACK(key),NULL);g_signal_connect(action,"button-press-event",G_CALLBACK(button),NULL);
    g_signal_connect(dialog,"response",G_CALLBACK(response),NULL);gtk_widget_show_all(dialog);
    for (unsigned retry=0;retry<400 && !drawn;++retry) {while (gtk_events_pending())gtk_main_iteration();if (!drawn)usleep(50000);}
    CHECK(drawn && gtk_widget_get_mapped(dialog),"real-drawing-mapped");
    CHECK(pango_layout_get_unknown_glyphs_count(gtk_label_get_layout(GTK_LABEL(label)))==0,"real-font-glyphs");
    while (gtk_events_pending())gtk_main_iteration();
    gdk_display_sync(gtk_widget_get_display(dialog));
    GdkWindow *gwindow=gtk_widget_get_window(dialog);CHECK(gwindow && GDK_IS_X11_WINDOW(gwindow),"real-x11-backend");
    Window window=GDK_WINDOW_XID(gwindow);CHECK(window,"actual-window-id");
    if (with_wm) {
        Window parent=None,returned_root,*children=NULL;unsigned count;
        for (unsigned retry=0;retry<1200 && (parent==None || parent==DefaultRootWindow(display));++retry) {
            while (gtk_events_pending())gtk_main_iteration();
            CHECK(XQueryTree(display,window,&returned_root,&parent,&children,&count),"query-real-frame-parent");
            if (children) {XFree(children);children=NULL;}
            if (parent==None || parent==DefaultRootWindow(display))usleep(50000);
        }
        CHECK(parent && parent!=DefaultRootWindow(display) && parent!=manager_window,"actual-xfwm4-reparenting");
        gdk_display_sync(gtk_widget_get_display(dialog));
        printf("NATIVE_XFCE_WM_READY:%s:%ld:%lu:%lu\n",nonce,(long)manager,manager_window,parent);fflush(stdout);
    }
    int origin_x,origin_y,area_x,area_y,button_x,button_y;
    gdk_window_get_origin(gwindow,&origin_x,&origin_y);
    CHECK(gtk_widget_translate_coordinates(area,dialog,0,0,&area_x,&area_y),"area-coordinates");
    CHECK(gtk_widget_translate_coordinates(action,dialog,gtk_widget_get_allocated_width(action)/2,gtk_widget_get_allocated_height(action)/2,&button_x,&button_y),"button-coordinates");
    XSetInputFocus(display,window,RevertToParent,CurrentTime);XChangePointerControl(display,True,True,1,1,0);XSync(display,False);
    Window child,root;int rx,ry,wx,wy;unsigned mask;
    CHECK(XQueryPointer(display,RootWindow(display,screen),&root,&child,&rx,&ry,&wx,&wy,&mask),"pointer-state");
    printf("NATIVE_XFCE_UI_READY:%s:%s:%s:%d:%d:%lu:%ld:%ld:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d\n",nonce,identity.release,identity.machine,
        width,height,window,(long)server,(long)getpid(),origin_x+area_x,origin_y+area_y,gtk_widget_get_allocated_width(area),gtk_widget_get_allocated_height(area),
        origin_x+button_x,origin_y+button_y,rx,ry,gtk_widget_get_allocated_width(dialog),gtk_widget_get_allocated_height(dialog));fflush(stdout);
    gboolean pointer_ready=FALSE;
    for (unsigned retry=0;retry<2400 && !(key_seen && response_seen);++retry) {
        while (gtk_events_pending())gtk_main_iteration();if (!(key_seen && response_seen))usleep(50000);
        if (!pointer_ready && XQueryPointer(display,RootWindow(display,screen),&root,&child,&rx,&ry,&wx,&wy,&mask) &&
            rx==origin_x+button_x && ry==origin_y+button_y) {
            pointer_ready=TRUE;printf("NATIVE_XFCE_UI_POINTER:%s:%d:%d\n",nonce,rx,ry);fflush(stdout);
        }
    }
    CHECK(key_seen && button_seen && response_seen,"hardware-input-timeout");gtk_widget_destroy(dialog);
    while (gtk_events_pending())gtk_main_iteration();XCloseDisplay(display);
    if (with_wm) {
        int status;pid_t waited,child=with_desktop?session:manager;
        if (with_desktop) {
            GVariant *logout=g_dbus_connection_call_sync(bus,"org.xfce.SessionManager","/org/xfce/SessionManager","org.xfce.Session.Manager","Logout",g_variant_new("(bb)",FALSE,FALSE),G_VARIANT_TYPE("()"),G_DBUS_CALL_FLAGS_NONE,120000,NULL,&error);
            CHECK(logout,"actual-session-logout");g_variant_unref(logout);g_object_unref(bus);
        } else CHECK(!kill(manager,SIGTERM),"stop-window-manager");
        do {waited=waitpid(child,&status,0);} while (waited<0 && errno==EINTR);
        CHECK(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"manager-or-session-clean-exit");
        g_object_unref(manager_configuration);xfconf_shutdown();
    }
    CHECK(!kill(server,SIGTERM),"stop-server");int status;pid_t waited;
    do {waited=waitpid(server,&status,0);} while (waited<0 && errno==EINTR);
    CHECK(waited==server && WIFEXITED(status) && WEXITSTATUS(status)==0,"server-clean-exit");
    printf("NATIVE_XFCE_UI_OK:%s:%s:%s\n",nonce,identity.release,identity.machine);return 0;
}

/* Real Xfce utility/RC semantics and independent native xfconfd persistence. */
#include <glib.h>
#include <gio/gio.h>
#include <libxfce4util/libxfce4util.h>
#include <xfconf/xfconf.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>

#define CHECK(c,label) do { if (!(c)) { fprintf(stderr,"NATIVE_XFCE_BASE_FAIL:%s\n",label); return 1; } } while (0)

static guint32 daemon_pid(GDBusConnection *bus)
{
    GError *error=NULL;
    GVariant *value=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus",
        "GetConnectionUnixProcessID",g_variant_new("(s)","org.xfce.Xfconf"),G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,10000,NULL,&error);
    guint32 pid=0;
    if (value) {g_variant_get(value,"(u)",&pid);g_variant_unref(value);}
    if (error) {fprintf(stderr,"XFCE_DAEMON_PID_ERROR:%s\n",error->message);g_error_free(error);}
    return pid;
}

int main(int argc,char **argv)
{
    CHECK(argc==2 && strlen(argv[1])==48,"nonce");
    struct utsname identity;CHECK(!uname(&identity),"uname");
    CHECK(libxfce4util_major_version==4 && libxfce4util_minor_version==20 && libxfce4util_micro_version==1,"real-util-version");
    gchar *rc_path=g_strdup_printf("/tmp/xfce-rc-%s",argv[1]);
    XfceRc *rc=xfce_rc_simple_open(rc_path,FALSE);CHECK(rc,"rc-open");
    xfce_rc_set_group(rc,"Native");xfce_rc_write_entry(rc,"nonce",argv[1]);xfce_rc_write_int_entry(rc,"integer",73);xfce_rc_write_bool_entry(rc,"boolean",TRUE);
    xfce_rc_close(rc);rc=xfce_rc_simple_open(rc_path,TRUE);CHECK(rc,"rc-reopen");xfce_rc_set_group(rc,"Native");
    CHECK(!strcmp(xfce_rc_read_entry(rc,"nonce",""),argv[1]) && xfce_rc_read_int_entry(rc,"integer",0)==73 && xfce_rc_read_bool_entry(rc,"boolean",FALSE),"real-rc-values");
    xfce_rc_close(rc);CHECK(!unlink(rc_path),"rc-unlink");g_free(rc_path);
    GError *error=NULL;CHECK(xfconf_init(&error),"xfconf-init");
    gchar *name=g_strdup_printf("native-%s",argv[1]);XfconfChannel *channel=xfconf_channel_new(name);CHECK(channel,"channel");
    CHECK(xfconf_channel_set_string(channel,"/Native/source",argv[1]),"set-string");
    CHECK(xfconf_channel_set_int(channel,"/Native/integer",73) && xfconf_channel_set_bool(channel,"/Native/boolean",TRUE),"set-values");
    GDBusConnection *bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);CHECK(bus,"real-session-bus");
    guint32 first=daemon_pid(bus);CHECK(first>1 && first!=(guint32)getpid(),"independent-daemon");
    gchar *query_argv[]={"/usr/bin/xfconf-query","-c",name,"-p","/Native/source",NULL},*output=NULL,*diagnostic=NULL;
    gint status;
    CHECK(g_spawn_sync(NULL,query_argv,NULL,0,NULL,NULL,&output,&diagnostic,&status,&error) && g_spawn_check_wait_status(status,&error),"independent-query");
    gchar *expected=g_strconcat(argv[1],"\n",NULL);CHECK(!strcmp(output,expected),"query-observed-real-daemon-value");
    g_free(output);g_free(diagnostic);g_free(expected);g_object_unref(channel);xfconf_shutdown();
    CHECK(!kill((pid_t)first,SIGTERM),"stop-first-daemon");
    gboolean owned=TRUE;
    for (unsigned retry=0;retry<200 && owned;++retry) {
        GVariant *reply=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","NameHasOwner",
            g_variant_new("(s)","org.xfce.Xfconf"),G_VARIANT_TYPE("(b)"),G_DBUS_CALL_FLAGS_NONE,10000,NULL,&error);
        CHECK(reply,"bus-owner-query");g_variant_get(reply,"(b)",&owned);g_variant_unref(reply);if (owned)usleep(10000);
    }
    CHECK(!owned,"daemon-name-released");
    gchar *file=g_build_filename(g_get_user_config_dir(),"xfce4","xfconf","xfce-perchannel-xml",NULL);
    gchar *basename=g_strconcat(name,".xml",NULL),*xml_path=g_build_filename(file,basename,NULL),*xml=NULL;
    CHECK(g_file_get_contents(xml_path,&xml,NULL,&error) && strstr(xml,argv[1]),"real-xml-persistence");g_free(xml);
    CHECK(xfconf_init(&error),"reinitialize-client");channel=xfconf_channel_new(name);CHECK(channel,"reopen-channel");
    gchar *saved=xfconf_channel_get_string(channel,"/Native/source",NULL);
    CHECK(saved && !strcmp(saved,argv[1]) && xfconf_channel_get_int(channel,"/Native/integer",0)==73 && xfconf_channel_get_bool(channel,"/Native/boolean",FALSE),"values-survive-daemon-restart");
    guint32 second=daemon_pid(bus);CHECK(second>1 && second!=first && second!=(guint32)getpid(),"actual-daemon-restarted");
    printf("NATIVE_XFCE_BASE_OK:%s:%s:%s:client=%ld:first=%u:second=%u:rc=1:query=1:xml=1\n",argv[1],identity.release,identity.machine,(long)getpid(),first,second);
    g_free(saved);g_object_unref(channel);xfconf_shutdown();g_object_unref(bus);g_free(name);g_free(file);g_free(basename);g_free(xml_path);
    return 0;
}

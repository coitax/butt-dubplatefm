/*
 * Session-bus stand-in for an MPRIS v2 player.
 *
 * Replies to org.freedesktop.DBus.Properties.Get the way MediaPlayer2.Player
 * specifies: PlaybackStatus is a string variant ("Playing", "Paused", or
 * "Stopped") and Metadata is an a{sv} variant. xesam:artist is an array of
 * strings, xesam:title is a string, and mpris:trackid is an object path.
 * A "-" title or no artist arguments omits that key, which players do when
 * the track has no such field.
 *
 * Usage: mpris_stub <well-known-name> <PlaybackStatus> <title-or-> [artist ...]
 * Prints READY on stdout after the well-known name is owned.
 */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_status;
static const char *g_title; /* NULL when omitted */
static char **g_artists;
static int g_nartists;

static int append_track_id(DBusMessageIter *arr) {
    DBusMessageIter dict, var;
    const char *key = "mpris:trackid";
    const char *path = "/org/example/Track/1";
    if (!dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &dict))
        return 0;
    if (!dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key))
        return 0;
    if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "o", &var))
        return 0;
    if (!dbus_message_iter_append_basic(&var, DBUS_TYPE_OBJECT_PATH, &path))
        return 0;
    if (!dbus_message_iter_close_container(&dict, &var))
        return 0;
    return dbus_message_iter_close_container(arr, &dict);
}

static int append_title(DBusMessageIter *arr) {
    DBusMessageIter dict, var;
    const char *key = "xesam:title";
    if (!dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &dict))
        return 0;
    if (!dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key))
        return 0;
    if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "s", &var))
        return 0;
    if (!dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &g_title))
        return 0;
    if (!dbus_message_iter_close_container(&dict, &var))
        return 0;
    return dbus_message_iter_close_container(arr, &dict);
}

static int append_artists(DBusMessageIter *arr) {
    DBusMessageIter dict, var, inner;
    const char *key = "xesam:artist";
    int i;
    if (!dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &dict))
        return 0;
    if (!dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key))
        return 0;
    if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "as", &var))
        return 0;
    if (!dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &inner))
        return 0;
    for (i = 0; i < g_nartists; i++) {
        const char *artist = g_artists[i];
        if (!dbus_message_iter_append_basic(&inner, DBUS_TYPE_STRING, &artist))
            return 0;
    }
    if (!dbus_message_iter_close_container(&var, &inner))
        return 0;
    if (!dbus_message_iter_close_container(&dict, &var))
        return 0;
    return dbus_message_iter_close_container(arr, &dict);
}

static void reply_string_variant(DBusConnection *conn, DBusMessage *msg, const char *value) {
    DBusMessage *reply;
    DBusMessageIter args, var;
    reply = dbus_message_new_method_return(msg);
    if (reply == NULL)
        return;
    dbus_message_iter_init_append(reply, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&args, &var);
    dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
}

static void reply_metadata(DBusConnection *conn, DBusMessage *msg) {
    DBusMessage *reply;
    DBusMessageIter args, var, arr;
    reply = dbus_message_new_method_return(msg);
    if (reply == NULL)
        return;
    dbus_message_iter_init_append(reply, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "a{sv}", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "{sv}", &arr);
    append_track_id(&arr);
    if (g_nartists > 0)
        append_artists(&arr);
    if (g_title != NULL)
        append_title(&arr);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&args, &var);
    dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
}

static void handle_message(DBusConnection *conn, DBusMessage *msg) {
    DBusMessageIter iter;
    const char *iface = NULL;
    const char *prop = NULL;

    if (!dbus_message_is_method_call(msg, "org.freedesktop.DBus.Properties", "Get"))
        return;
    if (!dbus_message_iter_init(msg, &iter))
        return;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
        return;
    dbus_message_iter_get_basic(&iter, &iface);
    if (!dbus_message_iter_next(&iter))
        return;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
        return;
    dbus_message_iter_get_basic(&iter, &prop);

    if (strcmp(iface, "org.mpris.MediaPlayer2.Player") != 0)
        return;

    if (strcmp(prop, "PlaybackStatus") == 0) {
        fprintf(stderr, "stub PlaybackStatus=%s\n", g_status);
        reply_string_variant(conn, msg, g_status);
    } else if (strcmp(prop, "Metadata") == 0) {
        fprintf(stderr, "stub Metadata title=%s artists=%d\n",
                g_title != NULL ? g_title : "(omitted)", g_nartists);
        reply_metadata(conn, msg);
    }
}

int main(int argc, char **argv) {
    DBusError err;
    DBusConnection *conn;
    int ret;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <bus-name> <status> <title-or-> [artist ...]\n", argv[0]);
        return 2;
    }
    g_status = argv[2];
    g_title = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    g_nartists = argc - 4;
    g_artists = argv + 4;

    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (conn == NULL) {
        fprintf(stderr, "bus: %s\n", err.message);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(conn, 0);
    ret = dbus_bus_request_name(conn, argv[1],
                                DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                &err);
    if (dbus_error_is_set(&err) || ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "name %s: %s (ret %d)\n", argv[1],
                dbus_error_is_set(&err) ? err.message : "not primary", ret);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    while (dbus_connection_read_write(conn, 500)) {
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(conn)) != NULL) {
            handle_message(conn, msg);
            dbus_message_unref(msg);
        }
    }
    return 0;
}

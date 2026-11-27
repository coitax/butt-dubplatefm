#include "currentTrack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dbus/dbus.h>

static DBusError err;
static DBusConnection* conn = NULL;

static bool dbus_init(void) {
    if(conn == NULL) {
        int ret;
        dbus_error_init(&err);
        conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
        if(dbus_error_is_set(&err)) {
            printf("Connection Error: %s\n", err.message);
            dbus_error_free(&err);
            return false;
        }
        
        ret = dbus_bus_request_name(conn, "me.melchor9000.butt",
            DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
        if(dbus_error_is_set(&err)) {
            printf("Name error: %s\n", err.message);
            dbus_error_free(&err);
        }
        if(ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            return false;
        }
    }
    return true;
}

struct DBusMethodCall {
    const char* target;
    const char* object;
    const char* interface;
    const char* method;
    void (*arg_iter)(void* data, DBusMessageIter* args);
    void* data;
};

static bool dbus_call_method(const struct DBusMethodCall* opt, unsigned n, ...) {
    DBusMessage* msg;
    DBusMessageIter args;
    DBusPendingCall* pending;

    va_list va;
    va_start(va, n);
    
    msg = dbus_message_new_method_call(opt->target, opt->object, opt->interface, opt->method);
    if(msg == NULL) {
        printf("Message Null\n");
        for(unsigned i = 0; i < n; i++) va_arg(va, void*);
        va_end(va);
        return false;
    }
    
    dbus_message_iter_init_append(msg, &args);
    
    for(unsigned i = 0; i < n; i++) {
        const char* arg = va_arg(va, const char*);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &arg);
    }
    
    va_end(va);
    dbus_connection_send_with_reply(conn, msg, &pending, -1);
    if(pending == NULL) {
        printf("Pending call null\n");
        return false;
    }
    
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
    
    dbus_pending_call_block(pending);
    msg = dbus_pending_call_steal_reply(pending);
    if(msg == NULL) {
        printf("Reply NULL\n");
        return false;
    }
    
    dbus_pending_call_unref(pending);

    if(dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_ERROR) {
        if(dbus_message_iter_init(msg, &args)) {
            do {
                if(opt->arg_iter) opt->arg_iter(opt->data, &args);
            } while(dbus_message_iter_next(&args));
        }

        dbus_message_unref(msg);
        return true;
    } else {
        dbus_message_unref(msg);
        return false;
    }
}

static void playbackstatus(void* data, DBusMessageIter* args) {
    DBusMessageIter var;
    char* str = NULL;
    char** value = (char**) data;
    if(*value != NULL || dbus_message_iter_get_arg_type(args) != DBUS_TYPE_VARIANT) return;
    dbus_message_iter_recurse(args, &var);
    if(dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&var, &str);
    *value = strdup(str);
}

static char* join_artists(DBusMessageIter* value) {
    DBusMessageIter list;
    char* joined;
    size_t len = 0;

    if(dbus_message_iter_get_arg_type(value) != DBUS_TYPE_ARRAY ||
       dbus_message_iter_get_element_type(value) != DBUS_TYPE_STRING) {
        return NULL;
    }

    joined = (char*) calloc(1, 1);
    if(joined == NULL) return NULL;

    dbus_message_iter_recurse(value, &list);
    while(dbus_message_iter_get_arg_type(&list) == DBUS_TYPE_STRING) {
        const char* str = NULL;
        dbus_message_iter_get_basic(&list, &str);
        size_t n = strlen(str);
        if(n) {
            size_t sep = len ? 2 : 0;
            char* grown = (char*) realloc(joined, len + sep + n + 1);
            if(grown == NULL) {
                free(joined);
                return NULL;
            }
            joined = grown;
            if(sep) memcpy(joined + len, ", ", sep);
            memcpy(joined + len + sep, str, n);
            len += sep + n;
            joined[len] = '\0';
        }
        dbus_message_iter_next(&list);
    }
    return joined;
}

static void metadata(void* data, DBusMessageIter* args) {
    DBusMessageIter var, arr, entry, value;
    char** values = (char**) data;

    if(dbus_message_iter_get_arg_type(args) != DBUS_TYPE_VARIANT) return;
    dbus_message_iter_recurse(args, &var);
    if(dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY ||
       dbus_message_iter_get_element_type(&var) != DBUS_TYPE_DICT_ENTRY) {
        return;
    }

    dbus_message_iter_recurse(&var, &arr);
    while(dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        const char* key = NULL;
        dbus_message_iter_recurse(&arr, &entry);
        if(dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
            if(dbus_message_iter_next(&entry) &&
               dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &value);
                if(values[1] == NULL && !strcmp("xesam:artist", key)) {
                    values[1] = join_artists(&value);
                } else if(values[0] == NULL && !strcmp("xesam:title", key) &&
                          dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
                    const char* title = NULL;
                    dbus_message_iter_get_basic(&value, &title);
                    values[0] = strdup(title);
                }
            }
        }
        dbus_message_iter_next(&arr);
    }
}

static char* get_mpris(const char* target) {
    char* playbackStatusValue = NULL;
    char* returnString = NULL;
    struct DBusMethodCall opt = {
        target,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get",
        playbackstatus,
        &playbackStatusValue
    };

    dbus_init();
    if(conn == NULL) return NULL;
    if(dbus_bus_name_has_owner(conn, target, &err)) {
        dbus_call_method(&opt, 2, "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
        if(playbackStatusValue != NULL && strcmp(playbackStatusValue, "Stopped")) {
            char* values[2] = { NULL, NULL };
            opt.arg_iter = metadata;
            opt.data = values;
            dbus_call_method(&opt, 2, "org.mpris.MediaPlayer2.Player", "Metadata");
            if(values[0] != NULL && values[1] != NULL) {
                size_t artistLen = strlen(values[1]), titleLen = strlen(values[0]);
                returnString = (char*) malloc(artistLen + 3 + titleLen + 1);
                if(returnString != NULL) {
                    memcpy(returnString, values[1], artistLen);
                    memcpy(returnString + artistLen, " - ", 3);
                    memcpy(returnString + artistLen + 3, values[0], titleLen);
                    returnString[artistLen + 3 + titleLen] = '\0';
                }
            }
            free(values[0]);
            free(values[1]);
        }
        free(playbackStatusValue);
    }
    if(dbus_error_is_set(&err)) dbus_error_free(&err);
    return returnString;
}

const char* getCurrentTrackRhythmbox() {
    return get_mpris("org.gnome.Rhythmbox3");
}

const char* getCurrentTrackClementine() {
    return get_mpris("org.mpris.MediaPlayer2.clementine");
}

const char* getCurrentTrackSpotify() {
    return get_mpris("org.mpris.MediaPlayer2.spotify");
}

const char* getCurrentTrackCantata() {
    return get_mpris("org.mpris.MediaPlayer2.cantata");
}

static bool banshee_query(const char* cmd, const char* prefix, char* buff, size_t size) {
    FILE* p = popen(cmd, "r");
    if(p == NULL) return false;
    memset(buff, 0, size);
    char* bread = fgets(buff, size, p);
    int ret = pclose(p);
    if(bread == NULL || ret != 0) return false;

    size_t len = strlen(buff);
    if(len > 0 && buff[len - 1] == '\n') buff[--len] = '\0';
    return strncmp(buff, prefix, strlen(prefix)) == 0;
}

const char* getCurrentTrackBanshee() {
    static const char artistPrefix[] = "artist: ";
    static const char titlePrefix[] = "title: ";
    char buff1[512], buff2[512];

    if(!banshee_query("banshee --query-artist", artistPrefix, buff1, sizeof(buff1)) ||
       !banshee_query("banshee --query-title", titlePrefix, buff2, sizeof(buff2))) {
        return NULL;
    }

    const char* artist = buff1 + strlen(artistPrefix);
    const char* title = buff2 + strlen(titlePrefix);
    size_t size1 = strlen(artist), size2 = strlen(title);
    char* str = (char*) malloc(size1 + 3 + size2 + 1);
    if(str == NULL) return NULL;
    memcpy(str, artist, size1);
    memcpy(str + size1, " - ", 3);
    memcpy(str + size1 + 3, title, size2);
    str[size1 + 3 + size2] = '\0';
    return str;
}

currentTrackFunction getCurrentTrackFunctionFromId(int i) {
    switch(i) {
        case 0: return &getCurrentTrackRhythmbox;
        case 1: return &getCurrentTrackBanshee;
        case 2: return &getCurrentTrackClementine;
        case 3: return &getCurrentTrackSpotify;
        case 4: return &getCurrentTrackCantata;
        default: return NULL;
    }
}

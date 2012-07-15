/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Karl Anderson <karl@2600hz.com>
 * Darren Schreiber <darren@2600hz.com>
 *
 *
 * mod_kazoo.c -- Socket Controlled Event Handler
 *
 */
#include <switch.h>
#include <ei.h>

#define MAX_ACL 100
#define CMD_BUFLEN 1024 * 1000
#define MAX_QUEUE_LEN 25000
#define MAX_MISSED 500

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime);
SWITCH_MODULE_DEFINITION(mod_kazoo, mod_kazoo_load, mod_kazoo_shutdown, mod_kazoo_runtime);

static char *MARKER = "1";

typedef enum {
        LFLAG_AUTHED = (1 << 0),
        LFLAG_RUNNING = (1 << 1),
        LFLAG_EVENTS = (1 << 2),
        LFLAG_LOG = (1 << 3),
        LFLAG_FULL = (1 << 4),
        LFLAG_MYEVENTS = (1 << 5),
        LFLAG_SESSION = (1 << 6),
        LFLAG_ASYNC = (1 << 7),
        LFLAG_STATEFUL = (1 << 8),
        LFLAG_OUTBOUND = (1 << 9),
        LFLAG_LINGER = (1 << 10),
        LFLAG_HANDLE_DISCO = (1 << 11),
        LFLAG_CONNECTED = (1 << 12),
        LFLAG_RESUME = (1 << 13),
        LFLAG_AUTH_EVENTS = (1 << 14),
        LFLAG_ALL_EVENTS_AUTHED = (1 << 15),
        LFLAG_ALLOW_LOG = (1 << 16)
} event_flag_t;

typedef enum {
        ERLANG_STRING = 0,
        ERLANG_BINARY
} erlang_encoding_t;

struct listener {
        uint32_t id;
        int clientfd;
        switch_queue_t *event_queue;
        switch_queue_t *log_queue;
        switch_memory_pool_t *pool;
        switch_mutex_t *flag_mutex;
        uint32_t flags;
        switch_log_level_t level;
        uint8_t event_list[SWITCH_EVENT_ALL + 1];
        switch_hash_t *event_hash;
        switch_thread_rwlock_t *rwlock;
        switch_core_session_t *session;
        int lost_events;
        int lost_logs;
        char remote_ip[50];
        char *peer_nodename;
        struct ei_cnode_s *ec;
        struct listener *next;
};

typedef struct listener listener_t;

static struct {
        switch_mutex_t *listener_mutex;
        switch_event_node_t *node;
        int debug;
} globals;

static struct {
        switch_socket_t *sock;
        switch_mutex_t *sock_mutex;
        listener_t *listeners;
        uint8_t ready;
} listen_list;

static struct {
        switch_mutex_t *mutex;
        char *ip;
        uint16_t port;
        int done;
        int threads;
        char *acl[MAX_ACL];
        uint32_t acl_count;
        uint32_t id;
        int nat_map;
        char *ei_cookie;
        char *ei_nodename;
        switch_bool_t ei_shortname;
        int ei_compat_rel;
        erlang_encoding_t encoding;
} prefs;

static uint32_t next_id(void)
{
        uint32_t id;
        switch_mutex_lock(globals.listener_mutex);
        id = ++prefs.id;
        switch_mutex_unlock(globals.listener_mutex);
        return id;
}

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ip, prefs.ip);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_cookie, prefs.ei_cookie);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ei_nodename, prefs.ei_nodename);


/* Function Definitions */
static void *SWITCH_THREAD_FUNC listener_run(switch_thread_t *thread, void *obj);
static void launch_listener_thread(listener_t *listener);
static void remove_listener(listener_t *listener);
static void kill_listener(listener_t *l);
static void kill_all_listeners(void);


static switch_status_t log_handler(const switch_log_node_t *node, switch_log_level_t level)
{
        listener_t *l;

        switch_mutex_lock(globals.listener_mutex);
        for (l = listen_list.listeners; l; l = l->next) {
                if (switch_test_flag(l, LFLAG_LOG) && l->level >= node->level) {
                        switch_log_node_t *dnode = switch_log_node_dup(node);

                        if (switch_queue_trypush(l->log_queue, dnode) == SWITCH_STATUS_SUCCESS) {
                                if (l->lost_logs) {
                                        int ll = l->lost_logs;
                                        switch_event_t *event;
                                        l->lost_logs = 0;
                                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Lost %d log lines!\n", ll);
                                        if (switch_event_create(&event, SWITCH_EVENT_TRAP) == SWITCH_STATUS_SUCCESS) {
                                                switch_event_add_header(event, SWITCH_STACK_BOTTOM, "info", "lost %d log lines", ll);
                                                switch_event_fire(&event);
                                        }
                                }
                        } else {
                                switch_log_node_free(&dnode);
                                if (++l->lost_logs > MAX_MISSED) {
                                        kill_listener(l);
                                }
                        }
                }
        }
        switch_mutex_unlock(globals.listener_mutex);

        return SWITCH_STATUS_SUCCESS;
}

// This will be removed, this is for reference
static switch_status_t expire_listener(listener_t ** listener)
{
        listener_t *l;

        if (!listener || !*listener)
                return SWITCH_STATUS_FALSE;
        l = *listener;

        if (!l->expire_time) {
                l->expire_time = switch_epoch_time_now(NULL);
        }

        if (switch_thread_rwlock_trywrlock(l->rwlock) != SWITCH_STATUS_SUCCESS) {
                return SWITCH_STATUS_FALSE;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(l->session), SWITCH_LOG_CRIT, "Stateful Listener %u has expired\n", l->id);

        flush_listener(*listener, SWITCH_TRUE, SWITCH_TRUE);
        switch_core_hash_destroy(&l->event_hash);

        if (l->allowed_event_hash) {
                switch_core_hash_destroy(&l->allowed_event_hash);
        }

        if (l->allowed_api_hash) {
                switch_core_hash_destroy(&l->allowed_api_hash);
        }


        switch_mutex_lock(l->filter_mutex);
        if (l->filters) {
                switch_event_destroy(&l->filters);
        }

        switch_mutex_unlock(l->filter_mutex);
        switch_thread_rwlock_unlock(l->rwlock);
        switch_core_destroy_memory_pool(&l->pool);

        *listener = NULL;
        return SWITCH_STATUS_SUCCESS;
}

static void event_handler(switch_event_t *event)
{
        switch_event_t *clone = NULL;
        listener_t *l, *lp, *last = NULL;
        time_t now = switch_epoch_time_now(NULL);

        switch_assert(event != NULL);

        if (!listen_list.ready) {
                return;
        }

        lp = listen_list.listeners;

        switch_mutex_lock(globals.listener_mutex);
        while (lp) {
                int send = 0;

                l = lp;
                lp = lp->next;

                if (l->event_list[SWITCH_EVENT_ALL]) {
                        send = 1;
                } else if ((l->event_list[event->event_id])) {
                        if (event->event_id != SWITCH_EVENT_CUSTOM || !event->subclass_name || (switch_core_hash_find(l->event_hash, event->subclass_name))) {
                                send = 1;
                        }
                }

                if (send && switch_test_flag(l, LFLAG_MYEVENTS)) {
                        char *uuid = switch_event_get_header(event, "unique-id");
                        if (!uuid || strcmp(uuid, switch_core_session_get_uuid(l->session))) {
                                send = 0;
                        }
                }

                if (send) {
                        if (switch_event_dup(&clone, event) == SWITCH_STATUS_SUCCESS) {
                                if (switch_queue_trypush(l->event_queue, clone) == SWITCH_STATUS_SUCCESS) {
                                        if (l->lost_events) {
                                                int le = l->lost_events;
                                                l->lost_events = 0;
                                                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(l->session), SWITCH_LOG_CRIT, "Lost %d events!\n", le);
                                                clone = NULL;
                                                if (switch_event_create(&clone, SWITCH_EVENT_TRAP) == SWITCH_STATUS_SUCCESS) {
                                                        switch_event_add_header(clone, SWITCH_STACK_BOTTOM, "info", "lost %d events", le);
                                                        switch_event_fire(&clone);
                                                }
                                        }
                                } else {
                                        if (++l->lost_events > MAX_MISSED) {
                                                kill_listener(l);
                                        }
                                        switch_event_destroy(&clone);
                                }
                        } else {
                                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(l->session), SWITCH_LOG_ERROR, "Memory Error!\n");
                        }
                }
                last = l;
        }
        switch_mutex_unlock(globals.listener_mutex);
}

static void close_socket(switch_socket_t ** sock)
{
        switch_mutex_lock(listen_list.sock_mutex);
        if (*sock) {
                switch_socket_shutdown(*sock, SWITCH_SHUTDOWN_READWRITE);
                switch_socket_close(*sock);
                *sock = NULL;
        }
        switch_mutex_unlock(listen_list.sock_mutex);
}

static void add_listener(listener_t *listener)
{
        /* add me to the listeners so I get events */
        switch_mutex_lock(globals.listener_mutex);
        listener->next = listen_list.listeners;
        listen_list.listeners = listener;
        switch_mutex_unlock(globals.listener_mutex);
}

static void flush_listener(listener_t *listener, switch_bool_t flush_log, switch_bool_t flush_events)
{
        void *pop;

        if (listener->log_queue) {
                while (switch_queue_trypop(listener->log_queue, &pop) == SWITCH_STATUS_SUCCESS) {
                        switch_log_node_t *dnode = (switch_log_node_t *) pop;
                        if (dnode) {
                                switch_log_node_free(&dnode);
                        }
                }
        }

        if (listener->event_queue) {
                while (switch_queue_trypop(listener->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
                        switch_event_t *pevent = (switch_event_t *) pop;
                        if (!pop)
                                continue;
                        switch_event_destroy(&pevent);
                }
        }
}

static void remove_listener(listener_t *listener)
{
        listener_t *l, *last = NULL;

        switch_mutex_lock(globals.listener_mutex);
        for (l = listen_list.listeners; l; l = l->next) {
                if (l == listener) {
                        if (last) {
                                last->next = l->next;
                        } else {
                                listen_list.listeners = l->next;
                        }
                }
                last = l;
        }
        switch_mutex_unlock(globals.listener_mutex);
}

static void kill_listener(listener_t *l)
{
        switch_clear_flag(l, LFLAG_RUNNING);
        if (l->sock) {
                switch_socket_shutdown(l->sock, SWITCH_SHUTDOWN_READWRITE);
                switch_socket_close(l->sock);
        }

}

static void kill_all_listeners(void)
{
        listener_t *l;

        switch_mutex_lock(globals.listener_mutex);
        for (l = listen_list.listeners; l; l = l->next) {
                kill_listener(l);
        }
        switch_mutex_unlock(globals.listener_mutex);
}


static listener_t *find_listener(uint32_t id)
{
        listener_t *l, *r = NULL;

        switch_mutex_lock(globals.listener_mutex);
        for (l = listen_list.listeners; l; l = l->next) {
                if (l->id && l->id == id && !l->expire_time) {
                        if (switch_thread_rwlock_tryrdlock(l->rwlock) == SWITCH_STATUS_SUCCESS) {
                                r = l;
                        }
                        break;
                }
        }
        switch_mutex_unlock(globals.listener_mutex);
        return r;
}

/*
static void *SWITCH_THREAD_FUNC api_exec(switch_thread_t *thread, void *obj)
{
}
*/

static void *SWITCH_THREAD_FUNC listener_run(switch_thread_t *thread, void *obj)
{
        listener_t *listener = (listener_t *) obj;

        switch_assert(listener != NULL);

        switch_mutex_lock(globals.listener_mutex);
        prefs.threads++;
        switch_mutex_unlock(globals.listener_mutex);

        add_listener(listener);

        while (!prefs.done && switch_test_flag(listener, LFLAG_RUNNING)) {
                switch_yield(500000);
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Listener is still running for node(%s) %s\n", listener->remote_ip, listener->peer_nodename);
        }

        remove_listener(listener);

        if (globals.debug > 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Shutting down listener for node(%s) %s\n", listener->remote_ip, listener->peer_nodename);
        }

        switch_thread_rwlock_wrlock(listener->rwlock);
        flush_listener(listener, SWITCH_TRUE, SWITCH_TRUE);

        if (listener->sock) {
                close_socket(&listener->sock);
        }

        switch_thread_rwlock_unlock(listener->rwlock);

        if (globals.debug > 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Node(%s) %s connection closed\n", listener->remote_ip, listener->peer_nodename);
        }

        switch_core_hash_destroy(&listener->event_hash);

        if (listener->session) {
                switch_channel_clear_flag(switch_core_session_get_channel(listener->session), CF_CONTROLLED);
                switch_clear_flag_locked(listener, LFLAG_SESSION);
                if (locked) {
                        switch_core_session_rwunlock(listener->session);
                }
        }

        if (listener->pool) {
                switch_memory_pool_t *pool = listener->pool;
                switch_core_destroy_memory_pool(&pool);
        }

        switch_mutex_lock(globals.listener_mutex);
        prefs.threads--;
        switch_mutex_unlock(globals.listener_mutex);

        return NULL;
}

/* Create a thread for the socket and launch it */
static void launch_listener_thread(listener_t *listener)
{
        switch_thread_t *thread;
        switch_threadattr_t *thd_attr = NULL;

        switch_threadattr_create(&thd_attr, listener->pool);
        switch_threadattr_detach_set(thd_attr, 1);
        switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
        switch_thread_create(&thread, thd_attr, listener_run, listener, listener->pool);
}

static int config(void)
{
        char *cf = "kazoo.conf";
        switch_xml_t cfg, xml, settings, param;

        memset(&prefs, 0, sizeof(prefs));

        if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
        } else {
                if ((settings = switch_xml_child(cfg, "settings"))) {
                        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
                                char *var = (char *) switch_xml_attr_soft(param, "name");
                                char *val = (char *) switch_xml_attr_soft(param, "value");

                                if (!strcmp(var, "listen-ip")) {
                                        set_pref_ip(val);
                                } else if (!strcmp(var, "listen-port")) {
                                        prefs.port = (uint16_t) atoi(val);
                                } else if (!strcmp(var, "cookie")) {
                                        set_pref_ei_cookie(val);
                                } else if (!strcmp(var, "cookie-file")) {
                                        if (read_cookie_from_file(val) == 1) {
                                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie from %s\n", val);
                                        }
                                } else if (!strcmp(var, "nodename")) {
                                        set_pref_ei_nodname(val);
                                } else if (!strcmp(var, "shortname")) {
                                        set_pref_ei_shortname(val);
                                } else if (!strcmp(var, "compat-rel")) {
                                        if (atoi(val) >= 7)
                                                prefs.ei_compat_rel = atoi(val);
                                        else
                                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid compatability release '%s' specified\n", val);
                                } else if (!strcmp(var, "debug")) {
                                        globals.debug = atoi(val);
                                } else if (!strcmp(var, "encoding")) {
                                        if (!strcasecmp(val, "string")) {
                                                prefs.encoding = ERLANG_STRING;
                                        } else if (!strcasecmp(val, "binary")) {
                                                prefs.encoding = ERLANG_BINARY;
                                        } else {
                                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid encoding strategy '%s' specified\n", val);
                                        }
                                } else if (!strcmp(var, "nat-map")) {
                                        if (switch_true(val) && switch_nat_get_type()) {
                                                prefs.nat_map = 1;
                                        }
                                } else if (!strcasecmp(var, "apply-inbound-acl") && ! zstr(val)) {
                                        if (prefs.acl_count < MAX_ACL) {
                                                prefs.acl[prefs.acl_count++] = strdup(val);
                                        } else {
                                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Max acl records of %d reached\n", MAX_ACL);
                                        }
                                }
                        }
                }
                switch_xml_free(xml);
        }

        if (zstr(prefs.ip)) {
                set_pref_ip("0.0.0.0");
        }

        if (!prefs.port) {
                prefs.port = 8031;
        }

        if (zstr(prefs.ei_cookie)) {
                int res;
                char *home_dir = getenv("HOME");
                char path_buf[1024];

                if (!zstr(home_dir)) {
                        /* $HOME/.erlang.cookie */
                        switch_snprintf(path_buf, sizeof(path_buf), "%s%s%s", home_dir, SWITCH_PATH_SEPARATOR, ".erlang.cookie");
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Checking for cookie at path: %s\n", path_buf);

                        res = read_cookie_from_file(path_buf);
                        if (res) {
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No cookie or valid cookie file specified, using default cookie\n");
                                set_pref_ei_cookie("ClueCon");
                        }
                }
        }

        if (!prefs.nodename) {
                set_pref_ei_nodename("freeswitch");
        }

        if (!prefs.nat_map) {
                prefs.nat_map = 0;
        }

        if (!prefs.port) {
                prefs.port = 8021;
        }

        return 0;
}

static int read_cookie_from_file(char *filename) {
        int fd;
        char cookie[MAXATOMLEN+1];
        char *end;
        struct stat buf;
        ssize_t res;

        if (!stat(filename, &buf)) {
                if ((buf.st_mode & S_IRWXG) || (buf.st_mode & S_IRWXO)) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s must only be accessible by owner only.\n", filename);
                        return 2;
                }
                if (buf.st_size > MAXATOMLEN) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s contains a cookie larger than the maximum atom size of %d.\n", filename, MAXATOMLEN);
                        return 2;
                }
                fd = open(filename, O_RDONLY);
                if (fd < 1) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to open cookie file %s : %d.\n", filename, errno);
                        return 2;
                }

                if ((res = read(fd, cookie, MAXATOMLEN)) < 1) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to read cookie file %s : %d.\n", filename, errno);
                }

                cookie[MAXATOMLEN] = '\0';

                /* replace any end of line characters with a null */
                if ((end = strchr(cookie, '\n'))) {
                        *end = '\0';
                }

                if ((end = strchr(cookie, '\r'))) {
                        *end = '\0';
                }

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Read %d bytes from cookie file %s.\n", (int)res, filename);

                set_pref_cookie(cookie);
                return 0;
        } else {
                /* don't error here, because we might be blindly trying to read $HOME/.erlang.cookie, and that can fail silently */
                return 1;
        }
}

SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load)
{
// FIX ME:        switch_api_interface_t *api_interface;

        memset(&globals, 0, sizeof(globals));

        /* initialize the listener mutex */
        switch_mutex_init(&globals.listener_mutex, SWITCH_MUTEX_NESTED, pool);

        /* initialize listen_list */
        memset(&listen_list, 0, sizeof(listen_list));
        switch_mutex_init(&listen_list.sock_mutex, SWITCH_MUTEX_NESTED, pool);

        /* bind to all switch events */
        if (switch_event_bind_removable(modname, SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY, event_handler, NULL, &globals.node) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
                return SWITCH_STATUS_GENERR;
        }

        /* bind to all logs */
        if (prefs.bind_to_logger) {
		// FIX ME: Move to function
              switch_log_bind_logger(log_handler, SWITCH_LOG_DEBUG, SWITCH_FALSE);
        }

        /* connect my internal structure to the blank pointer passed to me */
        *module_interface = switch_loadable_module_create_module_interface(pool, modname);

        /* create an api for cli debug commands */
// FIX ME:        SWITCH_ADD_API(api_interface, "kazoo", "kazoo information", api_exec, "<command> [<args>]");
// FIX ME:        SWITCH_ADD_API(api_interface, "kazoo_bind_logs", "kazoo information", api_exec, "<command> [<args>]");
// FIX ME:        switch_console_set_complete("add kazoo listeners");

        /* indicate that the module should continue to be loaded */
        return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kazoo_shutdown)
{
        int sanity = 0;

        prefs.done = 1;

        kill_all_listeners();
        switch_log_unbind_logger(log_handler);

        close_socket(&listen_list.sock);

        while (prefs.threads) {
                switch_yield(100000);
                kill_all_listeners();
                if (++sanity >= 200) {
                        break;
                }
        }

        switch_event_unbind(&globals.node);

        switch_safe_free(prefs.ip);
        switch_safe_free(prefs.ei_cookie);

        return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_kazoo_runtime)
{
        switch_memory_pool_t *pool = NULL, *listener_pool = NULL;
        switch_status_t status;
        switch_sockaddr_t *sa;
        switch_socket_t *inbound_socket = NULL;
        listener_t *listener;
        struct ei_cnode_s ec; /* erlang c node interface connection */
        ErlConnect conn;
        int clientfd;
        int epmdfd;

        if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out Of Memory: Oh My God! They killed Kenny! YOU BASTARDS!\n");
                return SWITCH_STATUS_TERM;
        }

        config();

        /* PART 1: Open our socket to the world so people can connect to us */
        while (!prefs.done) {
                status = switch_sockaddr_info_get(&sa, prefs.ip, SWITCH_UNSPEC, prefs.port, 0, pool);

                if (!status) {
                        status = switch_socket_create(&listen_list.sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool);
                }

                if (!status && listen_list.sock) {
                        status = switch_socket_opt_set(listen_list.sock, SWITCH_SO_REUSEADDR, 1);
                }

                if (!status && listen_list.sock) {
                        status = switch_socket_bind(listen_list.sock, sa);
                }

                if (!status && listen_list.sock) {
                        status = switch_socket_listen(listen_list.sock, 5);
                }

                if (!status && listen_list.sock) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Erlang connection acceptor listenting on %s:%u\n", prefs.ip, prefs.port);

                        if (prefs.nat_map) {
                                switch_nat_add_mapping(prefs.port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE);
                        }

                        break;
                } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error, could not listen on %s:%u\n", prefs.ip, prefs.port);
                        switch_yield(500000);
                }
        }

        if (prefs.ei_compat_rel) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Compatability with OTP R%d requested\n", prefs.ei_compat_rel);
                ei_set_compat_rel(prefs.ei_compat_rel);
        }

        /* try to initialize the erlang interface */
        if (SWITCH_STATUS_SUCCESS != initialise_ei(&ec, sa)) {
                prefs.done = 1;
        }

        /* return value is -1 for error, a descriptor pointing to epmd otherwise */
        if ((epmdfd = ei_publish(&ec, prefs.port)) == -1) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                          "Failed to start epmd, is it in the freeswith user $PATH? Try starting it yourself or run an erl shell with the -sname or -name option.  Shutting down.\n");
                prefs.done = 1;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Connected to epmd and published erlang cnode at %s\n", ec.thisnodename);

        listen_list.ready = 1;

        /* PART 2: Accept connections, negotiate cookies with other party, then spawn a new thread for each connection to us with it's own memory pool */
        /*   NOTE: Each thread is responsible for taking duplicated events and poping them from it's queue to the client, as well as handling requests for XML configs */
        /*         in addition to inbound messages from Erlang */
        while (!prefs.done) {
                /* zero out errno because ei_accept doesn't differentiate between a
                 * failed authentication or a socket failure, or a client version
                 * mismatch or a godzilla attack (and a godzilla attack is highly likely) */
                errno = 0;

                if ((clientfd = ei_accept_tmo(&ec, (int) listen_list.sock, &conn, 500)) == ERL_ERROR) {
                        if (prefs.done) {
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Erlang connection acceptor shutting down\n");
                                break;
                        } else if (erl_errno == ETIMEDOUT) {
                                continue;
                        } else if (errno) {
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Erlang connection acceptor socket error %d %d\n", erl_errno, errno);
                        } else {
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                                                                  "Erlang node connection failed - probably bad cookie or bad nodename\n");
                        }
                        continue;
                }

                /* Create memory pool for this listener thread */
                if (switch_core_new_memory_pool(&listener_pool) != SWITCH_STATUS_SUCCESS) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out of memory: Too bad drinking scotch isn't a paying job or Kenny's dad would be a millionare!\n");
                        break;
                }

                /* From the listener's memory pool, allocate some memory for the listener's own structure */
                if (!(listener = switch_core_alloc(listener_pool, sizeof(*listener)))) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory Error\n");
                        break;
                }

                /* paranoid and unnecessary clean up of our allocation */
                memset(listener, 0, sizeof(*listener));

                /* Create a mutex and some queues for the work we will be doing to process events */
                switch_thread_rwlock_create(&listener->rwlock, listener_pool);
                switch_queue_create(&listener->event_queue, MAX_QUEUE_LEN, listener_pool);
                switch_queue_create(&listener->log_queue, MAX_QUEUE_LEN, listener_pool);

                listener->clientfd = clientfd;

                listener->pool = listener_pool;
                listener_pool = NULL;

                listener->ec = switch_core_alloc(listener->pool, sizeof(ei_cnode));
                memcpy(listener->ec, ec, sizeof(ei_cnode));

                switch_set_flag(listener, LFLAG_FULL);
                switch_set_flag(listener, LFLAG_RUNNING);

                switch_mutex_init(&listener->flag_mutex, SWITCH_MUTEX_NESTED, listener->pool);

                switch_core_hash_init(&listener->event_hash, listener->pool);

                /* store the IP and node name we are talking with */
                switch_inet_ntop(AF_INET, conn.ipadr, listener->remote_ip, sizeof(listener->remote_ip));
                listener->peer_nodename = switch_core_strdup(listener->pool, conn.nodename);

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "New erlang connection from node(%s) %s\n", listener->remote_ip, listener->peer_nodename);

                /* Go do some real work - start the thread for this erlang listener connection! */
                launch_listener_thread(listener);
        }

        if (listen_list.sock) {
                close_socket(&listen_list.sock);
        }

        /* Close the port we reserved for uPnP/Switch behind firewall, if necessary */
        if (prefs.nat_map && switch_nat_get_type()) {
                switch_nat_del_mapping(prefs.port, SWITCH_NAT_TCP);
        }

        /* Free our memory pool for handling sockets */
        if (pool) {
                switch_core_destroy_memory_pool(&pool);
        }

        for (uint32_t x = 0; x < prefs.acl_count; x++) {
                switch_safe_free(prefs.acl[x]);
        }

        return SWITCH_STATUS_TERM;
}

switch_status_t initialise_ei(struct ei_cnode_s *ec, switch_sockaddr_t *sa)
{
        switch_status_t rv;
        struct hostent *nodehost;
        char thishostname[EI_MAXHOSTNAMELEN + 1] = "";
        char thisnodename[MAXNODELEN + 1];
        char thisalivename[EI_MAXALIVELEN + 1];
        //EI_MAX_COOKIE_SIZE+1
        char ipbuf[25];
        const char *ip_addr;
        char *atsign;

        /* copy the erlang interface nodename into something we can modify */
        strncpy(thisalivename, prefs.ei_nodename, EI_MAXALIVELEN);

        ip_addr = switch_get_addr(ipbuf, sizeof(ipbuf), sa);

        if ((atsign = strchr(thisalivename, '@'))) {
                /* we got a qualified node name, don't guess the host/domain */
                snprintf(thisnodename, MAXNODELEN + 1, "%s", prefs.ei_nodename);
                /* truncate the alivename at the @ */
                *atsign = '\0';
        } else {
                if ((nodehost = gethostbyaddr(ip_addr, sizeof(ip_addr), AF_INET))) {
                        memcpy(thishostname, nodehost->h_name, EI_MAXHOSTNAMELEN);
                }

                if (zstr_buf(thishostname) || !strncasecmp(prefs.ip, "0.0.0.0", 7)) {
                        gethostname(thishostname, EI_MAXHOSTNAMELEN);
                }

                if (prefs.ei_shortname) {
                        char *off;
                        if ((off = strchr(thishostname, '.'))) {
                                *off = '\0';
                        }
                } else {
                        if (!(_res.options & RES_INIT)) {
                                // init the resolver
                                res_init();
                        }
                        if (_res.dnsrch[0] && !zstr_buf(_res.dnsrch[0])) {
                                strncat(thishostname, ".", 1);
                                strncat(thishostname, _res.dnsrch[0], EI_MAXHOSTNAMELEN - strlen(thishostname));
                        }

                }
                snprintf(thisnodename, MAXNODELEN + 1, "%s@%s", prefs.ei_nodename, thishostname);
        }

        /* init the ec stuff */
        if (ei_connect_xinit(ec, thishostname, thisalivename, thisnodename, (Erl_IpAddr)ip_addr, prefs.ei_cookie, 0) < 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to initialize the erlang interface connection structure\n");
                return SWITCH_STATUS_FALSE;
        }

        return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */

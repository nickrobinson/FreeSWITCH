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

#define MAX_ACL 100

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
} prefs;

static void remove_listener(listener_t *listener);
static void kill_listener(listener_t *l, const char *message);
static void kill_all_listeners(void);

static uint32_t next_id(void)
{
        uint32_t id;
        switch_mutex_lock(globals.listener_mutex);
        id = ++prefs.id;
        switch_mutex_unlock(globals.listener_mutex);
        return id;
}

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_ip, prefs.ip);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_pref_pass, prefs.ei_cookie);

static void *SWITCH_THREAD_FUNC listener_run(switch_thread_t *thread, void *obj);
static void launch_listener_thread(listener_t *listener);

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
                                        kill_listener(l, NULL);
                                }
                        }
                }
        }
        switch_mutex_unlock(globals.listener_mutex);

        return SWITCH_STATUS_SUCCESS;
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
                                                kill_listener(l, NULL);
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

static void send_disconnect(listener_t *listener, const char *message)
{

        char disco_buf[512] = "";
        switch_size_t len, mlen;

        if (zstr(message)) {
                message = "Disconnected.\n";
        }

        mlen = strlen(message);

        if (listener->session) {
                switch_snprintf(disco_buf, sizeof(disco_buf), "Content-Type: text/disconnect-notice\n"
                                                "Controlled-Session-UUID: %s\n"
                                                "Content-Disposition: disconnect\n" "Content-Length: %d\n\n", switch_core_session_get_uuid(listener->session), mlen);
        } else {
                switch_snprintf(disco_buf, sizeof(disco_buf), "Content-Type: text/disconnect-notice\nContent-Length: %d\n\n", mlen);
        }

        len = strlen(disco_buf);
        switch_socket_send(listener->sock, disco_buf, &len);
        if (len > 0) {
                len = mlen;
                switch_socket_send(listener->sock, message, &len);
        }
}

static void kill_listener(listener_t *l, const char *message)
{

        if (message) {
                send_disconnect(l, message);
        }

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
                kill_listener(l, "The system is being shut down.\n");
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

struct api_command_struct {
        char *api_cmd;
        char *arg;
        listener_t *listener;
        char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
        int bg;
        int ack;
        int console_execute;
        switch_memory_pool_t *pool;
};

static void *SWITCH_THREAD_FUNC api_exec(switch_thread_t *thread, void *obj)
{

        struct api_command_struct *acs = (struct api_command_struct *) obj;
        switch_stream_handle_t stream = { 0 };
        char *reply, *freply = NULL;
        switch_status_t status;

        switch_mutex_lock(globals.listener_mutex);
        prefs.threads++;
        switch_mutex_unlock(globals.listener_mutex);


        if (!acs) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Internal error.\n");
                goto cleanup;
        }

        if (!acs->listener || !switch_test_flag(acs->listener, LFLAG_RUNNING) ||
                !acs->listener->rwlock || switch_thread_rwlock_tryrdlock(acs->listener->rwlock) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error! cannot get read lock.\n");
                acs->ack = -1;
                goto done;
        }

        acs->ack = 1;

        SWITCH_STANDARD_STREAM(stream);

        if (acs->console_execute) {
                if ((status = switch_console_execute(acs->api_cmd, 0, &stream)) != SWITCH_STATUS_SUCCESS) {
                        stream.write_function(&stream, "-ERR %s Command not found!\n", acs->api_cmd);
                }
        } else {
                status = switch_api_execute(acs->api_cmd, acs->arg, NULL, &stream);
        }

        if (status == SWITCH_STATUS_SUCCESS) {
                reply = stream.data;
        } else {
                freply = switch_mprintf("-ERR %s Command not found!\n", acs->api_cmd);
                reply = freply;
        }

        if (!reply) {
                reply = "Command returned no output!";
        }

        if (acs->bg) {
                switch_event_t *event;

                if (switch_event_create(&event, SWITCH_EVENT_BACKGROUND_JOB) == SWITCH_STATUS_SUCCESS) {
                        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-UUID", acs->uuid_str);
                        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-Command", acs->api_cmd);
                        if (acs->arg) {
                                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Job-Command-Arg", acs->arg);
                        }
                        switch_event_add_body(event, "%s", reply);
                        switch_event_fire(&event);
                }
        } else {
                switch_size_t rlen, blen;
                char buf[1024] = "";

                if (!(rlen = strlen(reply))) {
                        reply = "-ERR no reply\n";
                        rlen = strlen(reply);
                }

                switch_snprintf(buf, sizeof(buf), "Content-Type: api/response\nContent-Length: %" SWITCH_SSIZE_T_FMT "\n\n", rlen);
                blen = strlen(buf);
                switch_socket_send(acs->listener->sock, buf, &blen);
                switch_socket_send(acs->listener->sock, reply, &rlen);
        }

        switch_safe_free(stream.data);
        switch_safe_free(freply);

        if (acs->listener->rwlock) {
                switch_thread_rwlock_unlock(acs->listener->rwlock);
        }

  done:

        if (acs->bg) {
                switch_memory_pool_t *pool = acs->pool;
                if (acs->ack == -1) {
                        int sanity = 2000;
                        while (acs->ack == -1) {
                                switch_cond_next();
                                if (--sanity <= 0)
                                        break;
                        }
                }

                acs = NULL;
                switch_core_destroy_memory_pool(&pool);
                pool = NULL;

        }

  cleanup:
        switch_mutex_lock(globals.listener_mutex);
        prefs.threads--;
        switch_mutex_unlock(globals.listener_mutex);

        return NULL;

}

static void *SWITCH_THREAD_FUNC listener_run(switch_thread_t *thread, void *obj)
{
        listener_t *listener = (listener_t *) obj;
        char buf[1024];
        switch_size_t len;
        switch_status_t status;
        switch_event_t *event;
        char reply[512] = "";
        switch_core_session_t *session = NULL;
        switch_channel_t *channel = NULL;
        switch_event_t *revent = NULL;
        const char *var;
        int locked = 1;

        switch_mutex_lock(globals.listener_mutex);
        prefs.threads++;
        switch_mutex_unlock(globals.listener_mutex);

        switch_assert(listener != NULL);

        if ((session = listener->session)) {
                if (switch_core_session_read_lock(session) != SWITCH_STATUS_SUCCESS) {
                        locked = 0;
                        goto done;
                }
        }

        switch_socket_opt_set(listener->sock, SWITCH_SO_TCP_NODELAY, TRUE);
        switch_socket_opt_set(listener->sock, SWITCH_SO_NONBLOCK, TRUE);

        if (prefs.acl_count && listener->sa && !zstr(listener->remote_ip)) {
                uint32_t x = 0;

                for (x = 0; x < prefs.acl_count; x++) {
                        if (!switch_check_network_list_ip(listener->remote_ip, prefs.acl[x])) {
                                const char message[] = "Access Denied, go away.\n";
                                int mlen = strlen(message);

                                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "IP %s Rejected by acl \"%s\"\n", listener->remote_ip,
                                                                  prefs.acl[x]);

                                switch_snprintf(buf, sizeof(buf), "Content-Type: text/rude-rejection\nContent-Length: %d\n\n", mlen);
                                len = strlen(buf);
                                switch_socket_send(listener->sock, buf, &len);
                                len = mlen;
                                switch_socket_send(listener->sock, message, &len);
                                goto done;
                        }
                }
        }

        if (globals.debug > 0) {
                if (zstr(listener->remote_ip)) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Connection Open\n");
                } else {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Connection Open from %s:%d\n", listener->remote_ip,
                                                          listener->remote_port);
                }
        }

        switch_socket_opt_set(listener->sock, SWITCH_SO_NONBLOCK, TRUE);
        switch_set_flag_locked(listener, LFLAG_RUNNING);
        add_listener(listener);

        if (session && switch_test_flag(listener, LFLAG_AUTHED)) {
                switch_event_t *ievent = NULL;

                switch_set_flag_locked(listener, LFLAG_SESSION);
                status = read_packet(listener, &ievent, 25);

                if (status != SWITCH_STATUS_SUCCESS || !ievent) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Socket Error!\n");
                        switch_clear_flag_locked(listener, LFLAG_RUNNING);
                        goto done;
                }


                if (parse_command(listener, &ievent, reply, sizeof(reply)) != SWITCH_STATUS_SUCCESS) {
                        switch_clear_flag_locked(listener, LFLAG_RUNNING);
                        goto done;
                }


        } else {
                switch_snprintf(buf, sizeof(buf), "Content-Type: auth/request\n\n");

                len = strlen(buf);
                switch_socket_send(listener->sock, buf, &len);

                while (!switch_test_flag(listener, LFLAG_AUTHED)) {
                        status = read_packet(listener, &event, 25);
                        if (status != SWITCH_STATUS_SUCCESS) {
                                goto done;
                        }
                        if (!event) {
                                continue;
                        }

                        if (parse_command(listener, &event, reply, sizeof(reply)) != SWITCH_STATUS_SUCCESS) {
                                switch_clear_flag_locked(listener, LFLAG_RUNNING);
                                goto done;
                        }
                        if (*reply != '\0') {
                                if (*reply == '~') {
                                        switch_snprintf(buf, sizeof(buf), "Content-Type: command/reply\n%s", reply + 1);
                                } else {
                                        switch_snprintf(buf, sizeof(buf), "Content-Type: command/reply\nReply-Text: %s\n\n", reply);
                                }
                                len = strlen(buf);
                                switch_socket_send(listener->sock, buf, &len);
                        }
                        break;
                }
        }

        while (!prefs.done && switch_test_flag(listener, LFLAG_RUNNING) && listen_list.ready) {
                len = sizeof(buf);
                memset(buf, 0, len);
                status = read_packet(listener, &revent, 0);

                if (status != SWITCH_STATUS_SUCCESS) {
                        break;
                }

                if (!revent) {
                        continue;
                }

                if (parse_command(listener, &revent, reply, sizeof(reply)) != SWITCH_STATUS_SUCCESS) {
                        switch_clear_flag_locked(listener, LFLAG_RUNNING);
                        break;
                }

                if (revent) {
                        switch_event_destroy(&revent);
                }

                if (*reply != '\0') {
                        if (*reply == '~') {
                                switch_snprintf(buf, sizeof(buf), "Content-Type: command/reply\n%s", reply + 1);
                        } else {
                                switch_snprintf(buf, sizeof(buf), "Content-Type: command/reply\nReply-Text: %s\n\n", reply);
                        }
                        len = strlen(buf);
                        switch_socket_send(listener->sock, buf, &len);
                }

        }

  done:

        if (revent) {
                switch_event_destroy(&revent);
        }

        remove_listener(listener);

        if (globals.debug > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Session complete, waiting for children\n");
        }

        switch_thread_rwlock_wrlock(listener->rwlock);
        flush_listener(listener, SWITCH_TRUE, SWITCH_TRUE);
        switch_mutex_lock(listener->filter_mutex);
        if (listener->filters) {
                switch_event_destroy(&listener->filters);
        }
        switch_mutex_unlock(listener->filter_mutex);

        if (listener->session) {
                channel = switch_core_session_get_channel(listener->session);
        }

        if (channel && (switch_test_flag(listener, LFLAG_RESUME) || ((var = switch_channel_get_variable(channel, "socket_resume")) && switch_true(var)))) {
                switch_channel_set_state(channel, CS_RESET);
        }

        if (listener->sock) {
                send_disconnect(listener, "Disconnected, goodbye.\nSee you at ClueCon! http://www.cluecon.com/\n");
                close_socket(&listener->sock);
        }

        switch_thread_rwlock_unlock(listener->rwlock);

        if (globals.debug > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Connection Closed\n");
        }

        switch_core_hash_destroy(&listener->event_hash);

        if (listener->allowed_event_hash) {
                switch_core_hash_destroy(&listener->allowed_event_hash);
        }

        if (listener->allowed_api_hash) {
                switch_core_hash_destroy(&listener->allowed_api_hash);
        }

        if (listener->session) {
                switch_channel_clear_flag(switch_core_session_get_channel(listener->session), CF_CONTROLLED);
                switch_clear_flag_locked(listener, LFLAG_SESSION);
                if (locked) {
                        switch_core_session_rwunlock(listener->session);
                }
        } else if (listener->pool) {
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
        char *cf = "event_socket.conf";
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
                                } else if (!strcmp(var, "debug")) {
                                        globals.debug = atoi(val);
                                } else if (!strcmp(var, "nat-map")) {
                                        if (switch_true(val) && switch_nat_get_type()) {
                                                prefs.nat_map = 1;
                                        }
                                } else if (!strcmp(var, "listen-port")) {
                                        prefs.port = (uint16_t) atoi(val);
                                } else if (!strcmp(var, "ei_cookie")) {
                                        set_pref_pass(val);
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
                set_pref_ip("127.0.0.1");
        }

        if (zstr(prefs.ei_cookie)) {
                set_pref_pass("ClueCon");
        }

        if (!prefs.nat_map) {
                prefs.nat_map = 0;
        }

        if (prefs.nat_map) {
                prefs.nat_map = 0;
        }

        if (!prefs.port) {
                prefs.port = 8021;
        }

        return 0;
}


SWITCH_MODULE_LOAD_FUNCTION(mod_kazoo_load)
{
        switch_api_interface_t *api_interface;

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
        SWITCH_ADD_API(api_interface, "kazoo", "kazoo information", kazoo_api, "<command> [<args>]");
// FIX ME:        SWITCH_ADD_API(api_interface, "kazoo_bind_logs", "kazoo information", kazoo_api, "<command> [<args>]");
        switch_console_set_complete("add kazoo listeners");

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
        uint32_t x = 0;
        uint32_t errs = 0;

        if (switch_core_new_memory_pool(&pool) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Out Of Memory: Oh My God! They killed Kenny! YOU BASTARDS!\n");
                return SWITCH_STATUS_TERM;
        }

        config();

        /* PART 1: Open our socket to the world so people can connect to us */
        while (!prefs.done) {
                status = switch_sockaddr_info_get(&sa, prefs.ip, SWITCH_UNSPEC, prefs.port, 0, pool);
                if (status)
                        goto fail;
                status = switch_socket_create(&listen_list.sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, pool);
                if (status)
                        goto sock_fail;
                status = switch_socket_opt_set(listen_list.sock, SWITCH_SO_REUSEADDR, 1);
                if (status)
                        goto sock_fail;
                status = switch_socket_bind(listen_list.sock, sa);
                if (status)
                        goto sock_fail;
                status = switch_socket_listen(listen_list.sock, 5);
                if (status)
                        goto sock_fail;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Socket up listening on %s:%u\n", prefs.ip, prefs.port);

                if (prefs.nat_map) {
                        switch_nat_add_mapping(prefs.port, SWITCH_NAT_TCP, NULL, SWITCH_FALSE);
                }

                break;
          sock_fail:
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Socket Error! Could not listen on %s:%u\n", prefs.ip, prefs.port);
                switch_yield(100000);
        }

        if (prefs.ei_compat_rel) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Compatability with OTP R%d requested\n", prefs.ei_compat_rel);
                ei_set_compat_rel(prefs.ei_compat_rel);
        }

        /* try to initialize the erlang interface */
        if (SWITCH_STATUS_SUCCESS != initialise_ei(&ec, sa)) {
                goto init_failed;
        }

        /* return value is -1 for error, a descriptor pointing to epmd otherwise */
        if ((epmdfd = ei_publish(&ec, prefs.port)) == -1) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                          "Failed to start epmd, is it in the freeswith user $PATH? Try starting it yourself or run an erl shell with the -sname or -name option.  Shutting down.\n");
                goto init_failed;
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
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Shutting Down\n");
                                break;
                        } else if (erl_errno == ETIMEDOUT) {
                                continue;
                        } else if (errno) {
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Socket Error %d %d\n", erl_errno, errno);
                        } else {
                                /* if errno didn't get set, assume nothing *too* horrible occured */
                                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                                                                  "Someone tried to connect to us but there was an error in negotiation - probably bad cookie or bad nodename\n");
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
                switch_set_flag(listener, LFLAG_ALLOW_LOG);

                switch_mutex_init(&listener->flag_mutex, SWITCH_MUTEX_NESTED, listener->pool);

                switch_core_hash_init(&listener->event_hash, listener->pool);

                /* store the IP and node name we are talking with */
                switch_inet_ntop(AF_INET, conn.ipadr, listener->remote_ip, sizeof(listener->remote_ip));
                listener->peer_nodename = switch_core_strdup(listener->pool, conn.nodename);

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "New erlang connection from node(%s) %s\n", listener->remote_ip, listener->peer_nodename);

                /* Go do some real work - start the thread for this erlang listener connection! */
                launch_listener_thread(listener);
        }

  end:

        close_socket(&listen_list.sock);

        /* Close the port we reserved for uPnP/Switch behind firewall, if necessary */
        if (prefs.nat_map && switch_nat_get_type()) {
                switch_nat_del_mapping(prefs.port, SWITCH_NAT_TCP);
        }

        /* Free our memory pool for handling sockets */
        if (pool) {
                switch_core_destroy_memory_pool(&pool);
        }

        for (x = 0; x < prefs.acl_count; x++) {
                switch_safe_free(prefs.acl[x]);
        }

  fail:
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
        char *ampersand;

        /* copy the erlang interface nodename into something we can modify */
        strncpy(thisalivename, prefs.ei_nodename, MAXNODELEN);

        if ((ampersand = strchr(thisalivename, '@'))) {
                /* we got a qualified node name, don't guess the host/domain */
                snprintf(thisnodename, MAXNODELEN + 1, "%s", prefs.nodename);
                /* truncate the alivename at the @ */
                *ampersand = '\0';
        } else {
                if ((nodehost = gethostbyaddr((const char *) &server_addr.sin_addr.s_addr, sizeof(server_addr.sin_addr.s_addr), AF_INET))) {
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
        if (ei_connect_xinit(ec, thishostname, thisalivename, thisnodename, (Erl_IpAddr) (&server_addr.sin_addr.s_addr), prefs.ei_cookie, 0) < 0) {
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

#define MAX_ACL 100
#define CMD_BUFLEN 1024 * 1000
#define MAX_QUEUE_LEN 25000
#define MAX_MISSED 500

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

typedef enum {
	NONE = 0,
	ERLANG_PID,
	ERLANG_REG_PROCESS
} process_type;

struct erlang_process {
	process_type type;
	char *reg_name;
	erlang_pid pid;
};

struct xml_fetch_msg {
        switch_mutex_t *mutex;
        switch_xml_t xml;
        char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
        const char *section;
        const char *tag_name;
        const char *key_name;
        const char *key_value;
        switch_bool_t responded;
};

typedef struct xml_fetch_msg xml_fetch_msg_t;

struct xml_msg_list {
        xml_fetch_msg_t *xml_msg;
        struct xml_msg_list *next;
};

typedef struct xml_msg_list xml_msg_list_t;

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
        switch_hash_t *event_bindings;
        switch_hash_t *session_bindings;
        switch_hash_t *log_bindings;
        switch_hash_t *fetch_bindings;
        switch_thread_rwlock_t *rwlock;
        switch_core_session_t *session;
        int lost_events;
        int lost_logs;
        char remote_ip[50];
        char *peer_nodename;
	xml_msg_list_t *xml_msgs;
        struct ei_cnode_s *ec;
        struct listener *next;
};
typedef struct listener listener_t;

struct prefs_s { 
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
	switch_bool_t bind_to_logger;
} prefs;

typedef struct prefs_s prefs_t;

struct api_command_struct {
	char *api_cmd;
	char *arg;
	listener_t *listener;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	uint8_t bg;
	erlang_pid pid;
	switch_memory_pool_t *pool;
};

/* kazoo_binding.c */
switch_status_t add_event_binding(listener_t *listener, switch_event_types_t *type, erlang_pid *from);
switch_status_t list_event_bindings(listener_t *listener);

/* handle_msg.c */
switch_status_t handle_msg(listener_t *listener, erlang_msg * msg, ei_x_buff * buf, ei_x_buff * rbuf);

/* ei_helpers.c */
void ei_link(listener_t *listener, erlang_pid * from, erlang_pid * to);
void ei_encode_switch_event_headers(ei_x_buff * ebuf, switch_event_t *event, prefs_t *prefs);
void ei_encode_switch_event_tag(ei_x_buff * ebuf, switch_event_t *event, char *tag, prefs_t *prefs);
int ei_pid_from_rpc(struct ei_cnode_s *ec, int sockfd, erlang_ref * ref, char *module, char *function);
void ei_x_print_reg_msg(ei_x_buff * buf, char *dest, int send);
void ei_x_print_msg(ei_x_buff * buf, erlang_pid * pid, int send);
int ei_sendto(ei_cnode * ec, int fd, struct erlang_process *process, ei_x_buff * buf);
int ei_helper_send(listener_t *listener, erlang_pid* to, char* buf, int len);
void ei_hash_ref(erlang_ref * ref, char *output);
int ei_compare_pids(erlang_pid * pid1, erlang_pid * pid2);
int ei_decode_string_or_binary(char *buf, int *index, int maxlen, char *dst);
switch_status_t initialize_ei(struct ei_cnode_s *ec, switch_sockaddr_t *sa, prefs_t *prefs );

#define ei_encode_switch_event(_b, _e) ei_encode_switch_event_tag(_b, _e, "event")

/* crazy macro for toggling encoding type */
#define _ei_x_encode_string(buf, string) switch (prefs->encoding) { \
	case ERLANG_BINARY: \
		ei_x_encode_binary(buf, string, strlen(string)); \
		break; \
	default: \
		ei_x_encode_string(buf, string); \
		break; \
}

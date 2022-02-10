
#include "bench.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>
#include <nng/supplemental/util/options.h>
#include <nng/supplemental/util/platform.h>

#ifdef NNG_SUPP_TLS
#include <nng/supplemental/tls/tls.h>
static int init_dialer_tls(nng_dialer d, const char *cacert, const char *cert,
                           const char *key, const char *pass);
#endif

static void loadfile(const char *path, void **datap, size_t *lenp);
static void client_stop(int argc, char **argv);

#define ASSERT_NULL(p, fmt, ...)   \
    if ((p) != NULL) {             \
        fatal(fmt, ##__VA_ARGS__); \
    }

struct topic {
    struct topic *next;
    char *        val;
};

struct client_opts {
    enum client_type type;
    bool             verbose;
    size_t           parallel;
    atomic_ulong     msg_count;
    size_t           interval;
    uint8_t          version;
    char *           url;
    struct topic *   topic;
    size_t           topic_count;
    uint8_t          qos;
    bool             retain;
    char *           user;
    char *           passwd;
    char *           client_id;
    uint16_t         keepalive;
    bool             clean_session;
    uint8_t *        msg;
    size_t           msg_len;
    uint8_t *        will_msg;
    size_t           will_msg_len;
    uint8_t          will_qos;
    bool             will_retain;
    char *           will_topic;
    bool             enable_ssl;
    char *           cacert;
    size_t           cacert_len;
    char *           cert;
    size_t           cert_len;
    char *           key;
    size_t           key_len;
    char *           keypass;
};

typedef struct client_opts client_opts;

client_opts *opts = NULL;

enum options {
    OPT_HELP = 1,
    OPT_VERBOSE,
    OPT_PARALLEL,
    OPT_MSGCOUNT,
    OPT_INTERVAL,
    OPT_VERSION,
    OPT_URL,
    OPT_PUB,
    OPT_SUB,
    OPT_TOPIC,
    OPT_QOS,
    OPT_RETAIN,
    OPT_USER,
    OPT_PASSWD,
    OPT_CLIENTID,
    OPT_KEEPALIVE,
    OPT_CLEAN_SESSION,
    OPT_WILL_MSG,
    OPT_WILL_QOS,
    OPT_WILL_RETAIN,
    OPT_WILL_TOPIC,
    OPT_SECURE,
    OPT_CACERT,
    OPT_CERTFILE,
    OPT_KEYFILE,
    OPT_KEYPASS,
    OPT_MSG,
    OPT_FILE,
};

static nng_optspec cmd_opts[] = {
    { .o_name = "help", .o_short = 'h', .o_val = OPT_HELP },
    { .o_name = "verbose", .o_short = 'v', .o_val = OPT_VERBOSE },
    { .o_name  = "parallel",
      .o_short = 'n',
      .o_val   = OPT_PARALLEL,
      .o_arg   = true },
    { .o_name  = "interval",
      .o_short = 'i',
      .o_val   = OPT_INTERVAL,
      .o_arg   = true },
    { .o_name = "count", .o_short = 'C', .o_val = OPT_MSGCOUNT, .o_arg = true },
    { .o_name = "version", .o_short = 'V', .o_val = OPT_VERSION },
    { .o_name = "url", .o_val = OPT_URL, .o_arg = true },
    { .o_name = "topic", .o_short = 't', .o_val = OPT_TOPIC, .o_arg = true },
    { .o_name = "qos", .o_short = 'q', .o_val = OPT_QOS, .o_arg = true },
    { .o_name = "retain", .o_short = 'r', .o_val = OPT_RETAIN },
    { .o_name = "user", .o_short = 'u', .o_val = OPT_USER, .o_arg = true },
    { .o_name  = "password",
      .o_short = 'p',
      .o_val   = OPT_PASSWD,
      .o_arg   = true },
    { .o_name = "id", .o_short = 'I', .o_val = OPT_CLIENTID, .o_arg = true },
    { .o_name  = "keepalive",
      .o_short = 'k',
      .o_val   = OPT_KEEPALIVE,
      .o_arg   = true },
    { .o_name  = "clean_session",
      .o_short = 'c',
      .o_val   = OPT_CLEAN_SESSION,
      .o_arg   = true },
    { .o_name = "will-msg", .o_val = OPT_WILL_MSG, .o_arg = true },
    { .o_name = "will-qos", .o_val = OPT_WILL_QOS, .o_arg = true },
    { .o_name = "will-retain", .o_val = OPT_WILL_RETAIN },
    { .o_name = "will-topic", .o_val = OPT_WILL_TOPIC, .o_arg = true },
    { .o_name = "secure", .o_short = 's', .o_val = OPT_SECURE },
    { .o_name = "cacert", .o_val = OPT_CACERT, .o_arg = true },
    { .o_name = "key", .o_val = OPT_KEYFILE, .o_arg = true },
    { .o_name = "keypass", .o_val = OPT_KEYPASS, .o_arg = true },
    {
        .o_name  = "cert",
        .o_short = 'E',
        .o_val   = OPT_CERTFILE,
        .o_arg   = true,
    },

    { .o_name = "msg", .o_short = 'm', .o_val = OPT_MSG, .o_arg = true },
    { .o_name = "file", .o_short = 'f', .o_val = OPT_FILE, .o_arg = true },

    { .o_name = NULL, .o_val = 0 },
};

struct work {
    enum { INIT, RECV, RECV_WAIT, SEND_WAIT, SEND } state;
    nng_aio *    aio;
    nng_msg *    msg;
    nng_ctx      ctx;
    client_opts *opts;
};

static atomic_bool exit_signal = false;
static atomic_long send_count  = 0;
static atomic_long recv_count  = 0;

void fatal(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static void nng_fatal(const char *msg, int rv)
{
    fatal("%s:%s", msg, nng_strerror(rv));
}

static void help(enum client_type type)
{
    switch (type) {
    case PUB:
        printf("Usage: " APP_NAME " pub  <addr> "
               "[<topic>...] [<opts>...] [<src>]\n\n");
        break;
    case SUB:
        printf("Usage: " APP_NAME " sub  <addr> "
               "[<topic>...] [<opts>...]\n\n");
        break;
    case CONN:
        printf("Usage: " APP_NAME " conn  <addr> "
               "[<opts>...]\n\n");
        break;

    default:
        break;
    }

    printf("<addr> must be one or more of:\n");
    printf("  --url <url>                      The url for mqtt broker "
           "('mqtt-tcp://host:port' or 'tls+mqtt-tcp://host:port') \n");
    printf("                                   [default: "
           "mqtt-tcp://127.0.0.1:1883]\n");

    if (type == PUB || type == SUB) {
        printf("\n<topic> must be set:\n");
        printf("  -t, --topic <topic>              Topic for publish or "
               "subscribe\n");
    }

    printf("\n<opts> may be any of:\n");
    printf("  -V, --version <version: 3|4|5>   The MQTT version used by "
           "the client [default: 4]\n");
    printf("  -n, --parallel             	   The number of parallel for "
           "client [default: 1]\n");
    printf("  -v, --verbose              	   Enable verbose mode\n");
    printf("  -u, --user <user>                The username for "
           "authentication\n");
    printf("  -p, --password <password>        The password for "
           "authentication\n");
    printf("  -k, --keepalive <keepalive>      A keep alive of the client "
           "(in seconds) [default: 60]\n");
    if (type == PUB) {
        printf("  -m, --msg <message>              The message to "
               "publish\n");
        printf("  -C, --count <num>                Max count of "
               "publishing "
               "message [default: 1]\n");
        printf("  -i, --interval <ms>              Interval of "
               "publishing "
               "message (ms) [default: 0]\n");
        printf("  -I, --identifier <identifier>    The client identifier "
               "UTF-8 String (default randomly generated string)\n");
    }
    printf("  -q, --qos <qos>                  Quality of service for the "
           "corresponding topic [default: 0]\n");
    printf("  -r, --retain                     The message will be "
           "retained [default: false]\n");
    printf("  -c, --clean_session <true|false> Define a clean start for "
           "the connection [default: true]\n");
    printf("  --will-qos <qos>                 Quality of service level "
           "for the will message [default: 0]\n");
    printf("  --will-msg <message>             The payload of the will "
           "message\n");
    printf("  --will-topic <topic>             The topic of the will "
           "message\n");
    printf("  --will-retain                    Will message as retained "
           "message [default: false]\n");

    printf("  -s, --secure                     Enable TLS/SSL mode\n");
    printf("      --cacert <file>              CA certificates file path\n");
    printf("      -E, --cert <file>            Certificate file path\n");
    printf("      --key <file>                 Private key file path\n");
    printf("      --keypass <key password>     Private key password\n");

    if (type == PUB) {
        printf("\n<src> may be one of:\n");
        printf("  -m, --msg  <data>                \n");
        printf("  -f, --file <file>                \n");
    }
}

static int intarg(const char *val, int maxv)
{
    int v = 0;

    if (val[0] == '\0') {
        fatal("Empty integer argument.");
    }
    while (*val != '\0') {
        if (!isdigit(*val)) {
            fatal("Integer argument expected.");
        }
        v *= 10;
        v += ((*val) - '0');
        val++;
        if (v > maxv) {
            fatal("Integer argument too large.");
        }
    }
    if (v < 0) {
        fatal("Integer argument overflow.");
    }
    return (v);
}

struct topic **addtopic(struct topic **endp, const char *s)
{
    struct topic *t;

    if (((t = malloc(sizeof(*t))) == NULL) ||
        ((t->val = malloc(strlen(s) + 1)) == NULL)) {
        fatal("Out of memory.");
    }
    memcpy(t->val, s, strlen(s) + 1);
    t->next = NULL;
    *endp   = t;
    return (&t->next);
}

void freetopic(struct topic *endp)
{
    struct topic *t = endp;

    for (struct topic *t = endp; t != NULL; t = t->next) {
        if (t->val) {
            free(t->val);
            t->val = NULL;
        }
    }
    free(t);
}

int client_parse_opts(int argc, char **argv, client_opts *opts)
{
    int    idx = 0;
    char * arg;
    int    val;
    int    rv;
    size_t filelen = 0;

    struct topic **topicend;
    topicend = &opts->topic;

    while ((rv = nng_opts_parse(argc, argv, cmd_opts, &val, &arg, &idx)) == 0) {
        switch (val) {
        case OPT_HELP:
            help(opts->type);
            exit(0);
            break;
        case OPT_VERBOSE:
            opts->verbose = true;
            break;
        case OPT_PARALLEL:
            opts->parallel = intarg(arg, 1024000);
            break;
        case OPT_INTERVAL:
            opts->interval = intarg(arg, 10240000);
            break;
        case OPT_MSGCOUNT:
            opts->msg_count = intarg(arg, 10240000);
            break;
        case OPT_VERSION:
            opts->version = intarg(arg, 4);
            break;
        case OPT_URL:
            ASSERT_NULL(opts->url,
                        "URL (--url) may be specified "
                        "only once.");
            opts->url = nng_strdup(arg);
            break;
        case OPT_TOPIC:
            topicend = addtopic(topicend, arg);
            opts->topic_count++;
            break;
        case OPT_QOS:
            opts->qos = intarg(arg, 2);
            break;
        case OPT_RETAIN:
            opts->retain = true;
            break;
        case OPT_USER:
            ASSERT_NULL(opts->user,
                        "User (-u, --user) may be specified "
                        "only "
                        "once.");
            opts->user = nng_strdup(arg);
            break;
        case OPT_PASSWD:
            ASSERT_NULL(opts->passwd,
                        "Password (-p, --password) may be "
                        "specified "
                        "only "
                        "once.");
            opts->passwd = nng_strdup(arg);
            break;
        case OPT_CLIENTID:
            ASSERT_NULL(opts->client_id,
                        "Identifier (-I, --identifier) may be "
                        "specified "
                        "only "
                        "once.");
            opts->client_id = nng_strdup(arg);
            break;
        case OPT_KEEPALIVE:
            opts->keepalive = intarg(arg, 65535);
            break;
        case OPT_CLEAN_SESSION:
            opts->clean_session = strcasecmp(arg, "true") == 0;
            break;
        case OPT_WILL_MSG:
            ASSERT_NULL(opts->will_msg,
                        "Will_msg (--will-msg) may be specified "
                        "only "
                        "once.");
            opts->will_msg     = nng_strdup(arg);
            opts->will_msg_len = strlen(arg);
            break;
        case OPT_WILL_QOS:
            opts->will_qos = intarg(arg, 2);
            break;
        case OPT_WILL_RETAIN:
            opts->retain = true;
            break;
        case OPT_WILL_TOPIC:
            ASSERT_NULL(opts->will_topic,
                        "Will_topic (--will-topic) may be "
                        "specified "
                        "only "
                        "once.");
            opts->will_topic = nng_strdup(arg);
            break;
        case OPT_SECURE:
            opts->enable_ssl = true;
            break;
        case OPT_CACERT:
            ASSERT_NULL(opts->cacert,
                        "CA Certificate (--cacert) may be "
                        "specified only once.");
            loadfile(arg, (void **) &opts->cacert, &opts->cacert_len);
            break;
        case OPT_CERTFILE:
            ASSERT_NULL(opts->cert,
                        "Cert (--cert) may be specified "
                        "only "
                        "once.");
            loadfile(arg, (void **) &opts->cert, &opts->cert_len);
            break;
        case OPT_KEYFILE:
            ASSERT_NULL(opts->key, "Key (--key) may be specified only once.");
            loadfile(arg, (void **) &opts->key, &opts->key_len);
            break;
        case OPT_KEYPASS:
            ASSERT_NULL(opts->keypass,
                        "Key Password (--keypass) may be specified only "
                        "once.");
            opts->keypass = nng_strdup(arg);
            break;
        case OPT_MSG:
            ASSERT_NULL(opts->msg,
                        "Data (--file, --data) may be "
                        "specified "
                        "only once.");
            opts->msg     = nng_strdup(arg);
            opts->msg_len = strlen(arg);
            break;
        case OPT_FILE:
            ASSERT_NULL(opts->msg,
                        "Data (--file, --data) may be "
                        "specified "
                        "only once.");
            loadfile(arg, (void **) &opts->msg, &opts->msg_len);
            break;
        }
    }
    switch (rv) {
    case NNG_EINVAL:
        fatal("Option %s is invalid.", argv[idx]);
        break;
    case NNG_EAMBIGUOUS:
        fatal("Option %s is ambiguous (specify in full).", argv[idx]);
        break;
    case NNG_ENOARG:
        fatal("Option %s requires argument.", argv[idx]);
        break;
    default:
        break;
    }

    if (!opts->url) {
        opts->url = nng_strdup("mqtt-tcp://127.0.0.1:1883");
    }

    switch (opts->type) {
    case PUB:
        if (opts->topic_count == 0) {
            fatal("Missing required option: '(-t, --topic) "
                  "<topic>'\nTry '" APP_NAME " pub --help' for more "
                  "information. ");
        }

        if (opts->msg == NULL) {
            fatal("Missing required option: '(-m, --msg) "
                  "<message>' or '(-f, --file) <file>'\nTry "
                  "'" APP_NAME " pub --help' for more information. ");
        }
        break;
    case SUB:
        if (opts->topic_count == 0) {
            fatal("Missing required option: '(-t, --topic) "
                  "<topic>'\nTry '" APP_NAME " sub --help' for more "
                  "information. ");
        }
        /* code */
        break;
    case CONN:
        /* code */
        break;

    default:
        break;
    }

    return rv;
}

static void set_default_conf(client_opts *opts)
{
    opts->msg_count     = 0;
    opts->interval      = 0;
    opts->qos           = 0;
    opts->retain        = false;
    opts->parallel      = 1;
    opts->version       = 4;
    opts->keepalive     = 60;
    opts->clean_session = true;
    opts->enable_ssl    = false;
    opts->verbose       = false;
    opts->topic_count   = 0;
}

// This reads a file into memory.  Care is taken to ensure that
// the buffer is one byte larger and contains a terminating
// NUL. (Useful for key files and such.)
static void loadfile(const char *path, void **datap, size_t *lenp)
{
    FILE * f;
    size_t total_read      = 0;
    size_t allocation_size = BUFSIZ;
    char * fdata;
    char * realloc_result;

    if (strcmp(path, "-") == 0) {
        f = stdin;
    } else {
        if ((f = fopen(path, "rb")) == NULL) {
            fatal("Cannot open file %s: %s", path, strerror(errno));
        }
    }

    if ((fdata = malloc(allocation_size + 1)) == NULL) {
        fatal("Out of memory.");
    }

    while (1) {
        total_read +=
            fread(fdata + total_read, 1, allocation_size - total_read, f);
        if (ferror(f)) {
            if (errno == EINTR) {
                continue;
            }
            fatal("Read from %s failed: %s", path, strerror(errno));
        }
        if (feof(f)) {
            break;
        }
        if (total_read == allocation_size) {
            if (allocation_size > SIZE_MAX / 2) {
                fatal("Out of memory.");
            }
            allocation_size *= 2;
            if ((realloc_result = realloc(fdata, allocation_size + 1)) ==
                NULL) {
                free(fdata);
                fatal("Out of memory.");
            }
            fdata = realloc_result;
        }
    }
    if (f != stdin) {
        fclose(f);
    }
    fdata[total_read] = '\0';
    *datap            = fdata;
    *lenp             = total_read;
}

#ifdef NNG_SUPP_TLS
static int init_dialer_tls(nng_dialer d, const char *cacert, const char *cert,
                           const char *key, const char *pass)
{
    nng_tls_config *cfg;
    int             rv;

    if ((rv = nng_tls_config_alloc(&cfg, NNG_TLS_MODE_CLIENT)) != 0) {
        return (rv);
    }

    if (cert != NULL && key != NULL) {
        nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_REQUIRED);
        if ((rv = nng_tls_config_own_cert(cfg, cert, key, pass)) != 0) {
            goto out;
        }
    } else {
        nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_NONE);
    }

    if (cacert != NULL) {
        if ((rv = nng_tls_config_ca_chain(cfg, cacert, NULL)) != 0) {
            goto out;
        }
    }

    rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, cfg);

out:
    nng_tls_config_free(cfg);
    return (rv);
}

#endif

nng_msg *publish_msg(client_opts *opts)
{
    // create a PUBLISH message
    nng_msg *pubmsg;
    nng_mqtt_msg_alloc(&pubmsg, 0);
    nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
    nng_mqtt_msg_set_publish_qos(pubmsg, opts->qos);
    nng_mqtt_msg_set_publish_retain(pubmsg, opts->retain);
    nng_mqtt_msg_set_publish_payload(pubmsg, opts->msg, opts->msg_len);
    nng_mqtt_msg_set_publish_topic(pubmsg, opts->topic->val);

    return pubmsg;
}

void client_cb(void *arg)
{
    struct work *work = arg;
    nng_msg *    msg;
    int          rv;

    switch (work->state) {
    case INIT:
        switch (work->opts->type) {
        case PUB:
            if (work->opts->msg_count > 0) {
                if (--send_count < 0) {
                    break;
                }
            }
            work->msg = publish_msg(work->opts);
            nng_msg_dup(&msg, work->msg);
            nng_aio_set_msg(work->aio, msg);
            msg         = NULL;
            work->state = SEND;
            nng_ctx_send(work->ctx, work->aio);
            // nng_sleep_aio(0, work->aio);
            break;
        case SUB:
        case CONN:
            work->state = RECV;
            nng_ctx_recv(work->ctx, work->aio);
            break;
        }
        break;

    case RECV:
        if ((rv = nng_aio_result(work->aio)) != 0) {
            nng_fatal("nng_recv_aio", rv);
            work->state = RECV;
            nng_ctx_recv(work->ctx, work->aio);
            break;
        }
        work->msg   = nng_aio_get_msg(work->aio);
        work->state = RECV_WAIT;
        // nng_sleep_aio(0, work->aio);
        nng_aio_finish(work->aio, 0);
        break;

    case RECV_WAIT:
        msg = work->msg;
        // uint32_t payload_len;
        // uint8_t *payload = nng_mqtt_msg_get_publish_payload(msg,
        // &payload_len); uint32_t topic_len; const char *recv_topic =
        //     nng_mqtt_msg_get_publish_topic(msg, &topic_len);

        // printf("%.*s: %.*s\n", topic_len, recv_topic, payload_len,
        //        (char *) payload);
        recv_count++;
        nng_msg_header_clear(work->msg);
        nng_msg_clear(work->msg);

        work->state = RECV;
        nng_ctx_recv(work->ctx, work->aio);
        break;

    case SEND:
        if ((rv = nng_aio_result(work->aio)) != 0) {
            nng_msg_free(work->msg);
            nng_fatal("nng_send_aio", rv);
        }
        nng_msg_dup(&msg, work->msg);
        nng_aio_set_msg(work->aio, msg);
        work->state = SEND_WAIT;
        if (work->opts->interval) {
            nng_sleep_aio(work->opts->interval, work->aio);
        } else {
            nng_aio_finish(work->aio, 0);
        }
        break;

    case SEND_WAIT:
        if (work->opts->msg_count > 0) {
            if (--send_count < 0) {
                goto out;
            }
        }
        if (work->opts->interval == 0) {
            goto out;
        }
        work->state = SEND;
        nng_ctx_send(work->ctx, work->aio);
        break;

    default:
        nng_fatal("bad state!", NNG_ESTATE);
        break;

    out:
        exit_signal = true;
        break;
    }
}

static struct work *alloc_work(nng_socket sock, client_opts *opts)
{
    struct work *w;
    int          rv;

    if ((w = nng_alloc(sizeof(*w))) == NULL) {
        nng_fatal("nng_alloc", NNG_ENOMEM);
    }
    if ((rv = nng_aio_alloc(&w->aio, client_cb, w)) != 0) {
        nng_fatal("nng_aio_alloc", rv);
    }
    if ((rv = nng_ctx_open(&w->ctx, sock)) != 0) {
        nng_fatal("nng_ctx_open", rv);
    }
    w->opts  = opts;
    w->state = INIT;
    return (w);
}

static nng_msg *connect_msg(client_opts *opts)
{
    nng_msg *msg;
    nng_mqtt_msg_alloc(&msg, 0);
    nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
    nng_mqtt_msg_set_connect_proto_version(msg, opts->version);
    nng_mqtt_msg_set_connect_keep_alive(msg, opts->keepalive);
    nng_mqtt_msg_set_connect_clean_session(msg, opts->clean_session);

    if (opts->client_id) {
        nng_mqtt_msg_set_connect_client_id(msg, opts->client_id);
    }
    if (opts->user) {
        nng_mqtt_msg_set_connect_user_name(msg, opts->user);
    }
    if (opts->passwd) {
        nng_mqtt_msg_set_connect_password(msg, opts->passwd);
    }
    if (opts->will_topic) {
        nng_mqtt_msg_set_connect_will_topic(msg, opts->will_topic);
    }
    if (opts->will_qos) {
        nng_mqtt_msg_set_connect_will_qos(msg, opts->will_qos);
    }
    if (opts->will_msg) {
        nng_mqtt_msg_set_connect_will_msg(msg, opts->will_msg,
                                          opts->will_msg_len);
    }
    if (opts->will_retain) {
        nng_mqtt_msg_set_connect_will_retain(msg, opts->will_retain);
    }

    return msg;
}

struct connect_param {
    nng_socket * sock;
    client_opts *opts;
};

void
connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	printf("%s: connected!\n", __FUNCTION__);
    struct connect_param *param    = arg;
    // uint8_t               ret_code = nng_mqtt_msg_get_connack_return_code(msg);
    // printf("%s(%d)\n",
    //        ret_code == 0 ? "connection established" : "connect failed",
    //        ret_code);

    // nng_msg_free(msg);
    // msg = NULL;

    // if (ret_code == 0) {
        if (param->opts->type == SUB && param->opts->topic_count > 0) {
            // Connected succeed
            nng_msg *msg;
            nng_mqtt_msg_alloc(&msg, 0);
            nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

            nng_mqtt_topic_qos *topics_qos =
                nng_mqtt_topic_qos_array_create(param->opts->topic_count);

            size_t i = 0;
            for (struct topic *tp = param->opts->topic;
                 tp != NULL && i < param->opts->topic_count;
                 tp = tp->next, i++) {
                nng_mqtt_topic_qos_array_set(topics_qos, i, tp->val,
                                             param->opts->qos);
            }

            nng_mqtt_msg_set_subscribe_topics(msg, topics_qos,
                                              param->opts->topic_count);

            nng_mqtt_topic_qos_array_free(topics_qos, param->opts->topic_count);

            // Send subscribe message
            nng_sendmsg(*param->sock, msg, NNG_FLAG_NONBLOCK);
        }
    // } else {
    //     fatal("connect failed: %d", ret_code);
    // }
}

// Disconnect message callback function
void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	printf("%s: disconnected!\n", __FUNCTION__);
}

void client(int argc, char **argv, enum client_type type)
{
    int rv;
    opts = nng_alloc(sizeof(client_opts));
    set_default_conf(opts);
    opts->type = type;

    client_parse_opts(argc, argv, opts);

    send_count            = opts->msg_count;
    atomic_long msg_count = opts->msg_count;
    if (opts->interval == 0 && opts->msg_count > 0) {
        opts->interval = 1;
    }

    nng_socket    sock;
    nng_dialer    dialer;
    struct work **works = nng_alloc(sizeof(struct work *) * opts->parallel);

    if ((rv = nng_mqtt_client_open(&sock)) != 0) {
        nng_fatal("nng_socket", rv);
    }

    for (size_t i = 0; i < opts->parallel; i++) {
        works[i] = alloc_work(sock, opts);
    }

    nng_msg *msg = connect_msg(opts);

    if ((rv = nng_dialer_create(&dialer, sock, opts->url)) != 0) {
        nng_fatal("nng_dialer_create", rv);
    }
    struct connect_param connect_arg = { .sock = &sock, .opts = opts };
	nng_mqtt_set_connect_cb(sock, connect_cb, &connect_arg);
	nng_mqtt_set_disconnect_cb(sock, disconnect_cb, NULL);
#ifdef NNG_SUPP_TLS
    if (opts->enable_ssl) {
        if ((rv = init_dialer_tls(dialer, opts->cacert, opts->cert, opts->key,
                                  opts->keypass)) != 0) {
            fatal("init_dialer_tls", rv);
        }
    }
#endif


    nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg);
    nng_dialer_start(dialer, NNG_FLAG_NONBLOCK);

    nng_time sleep_time = 1000;
    nng_time start      = nng_clock();

    for (size_t i = 0; i < opts->parallel; i++) {
        client_cb(works[i]);
    }
    nng_time used_time = 0;
    uint64_t total     = 0;

    uint64_t last_recv = 0;
    uint64_t temp      = 0;

    while (!exit_signal) {
        nng_msleep(sleep_time);
        used_time = nng_clock() - start - sleep_time;
        if (used_time > 0) {
            // used_time = used_time == 0 ? 1 : used_time;
            switch (opts->type) {
            case PUB:
                /* code */
                total = msg_count == 0
                    ? 1
                    : msg_count - send_count - opts->parallel;
                printf("sent total: %ld, rate: %lf(msg/sec), time: %ldms\n",
                       total, (total * 1000.0 / used_time), used_time);

                break;

            case SUB:
                /* code */
                if (last_recv != recv_count) {
                    total     = recv_count;
                    temp      = last_recv;
                    last_recv = recv_count;
                    printf("recv total: %ld, rate: %ld(msg/sec), time: %ld\n",
                           recv_count, total - temp, used_time);
                }
                break;

            default:
                break;
            }
        }
    }

    // printf("used time: %ldms\n", used_time);
    // used_time  = used_time == 0 ? 1 : used_time;
    // long total = msg_count == 0 ? 1 : msg_count - send_count -
    // opts->parallel; printf("total: %ld, rate: %lf(msg/sec)\n", total,
    //        (total * 1000.0 / used_time));

    nng_free(works, sizeof(struct work *) * opts->parallel);
    client_stop(argc, argv);
}

void client_stop(int argc, char **argv)
{
    if (opts) {
        if (opts->url) {
            nng_strfree(opts->url);
        }
        if (opts->topic) {
            freetopic(opts->topic);
        }
        if (opts->user) {
            nng_strfree(opts->user);
        }
        if (opts->passwd) {
            nng_strfree(opts->passwd);
        }
        if (opts->client_id) {
            nng_strfree(opts->client_id);
        }
        if (opts->msg) {
            nng_free(opts->msg, opts->msg_len);
        }
        if (opts->will_msg) {
            nng_free(opts->will_msg, opts->will_msg_len);
        }
        if (opts->will_topic) {
            nng_strfree(opts->will_topic);
        }
        if (opts->cacert) {
            nng_free(opts->cacert, opts->cacert_len);
        }
        if (opts->cert) {
            nng_free(opts->cert, opts->cert_len);
        }
        if (opts->key) {
            nng_free(opts->key, opts->key_len);
        }
        if (opts->keypass) {
            nng_strfree(opts->keypass);
        }

        free(opts);
    }
}

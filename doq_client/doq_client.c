/**
 * DNS-over-QUIC Client
 * Connects to AdGuard DoQ server and queries google.com
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <inttypes.h>

#include <xquic/xquic.h>
#include "dns_proto.h"
#include "event_loop.h"

/* DoQ Server Configuration
 * Tested servers:
 * - 223.6.6.6 (Alibaba Cloud): Handshake successful
 * - 1.1.1.1 (Cloudflare): Network timeout
 * - dns.google (Google): Network timeout
 */
#define DOQ_SERVER "94.140.15.15"
#define DOQ_PORT 853  /* RFC 9250 specifies port 853 for DoQ */
#define DOQ_ALPN "doq"
#define QUERY_DOMAIN "google.com"

#define PACKET_BUF_SIZE 1500
#define DNS_BUF_SIZE 512

/* Client context */
typedef struct {
    xqc_engine_t *engine;
    event_loop_t *event_loop;
    int sockfd;
    
    /* Connection info */
    xqc_cid_t cid;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen;
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen;
    
    /* Stream info */
    xqc_stream_t *stream;
    int handshake_complete;
    int query_sent;
    int response_received;
    
    /* DNS query/response */
    uint8_t query_buf[DNS_BUF_SIZE];
    size_t query_len;
    uint8_t response_buf[DNS_BUF_SIZE];
    size_t response_len;
    size_t response_offset;
    uint16_t expected_dns_len;
} doq_client_t;

static doq_client_t *g_client = NULL;

/* Forward declarations */
static int doq_send_query(doq_client_t *client);
static void doq_process_response(doq_client_t *client);

/*
 * ========================================================================
 * XQUIC Callbacks
 * ========================================================================
 */

/* Socket write callback - send UDP packet */
static ssize_t doq_write_socket(const unsigned char *buf, size_t size,
                                const struct sockaddr *peer_addr,
                                socklen_t peer_addrlen, void *user_data)
{
    doq_client_t *client = (doq_client_t *)user_data;
    ssize_t sent;
    
    sent = sendto(client->sockfd, buf, size, 0, peer_addr, peer_addrlen);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("[DoQ] sendto: EAGAIN\n");
            return -XQC_EAGAIN;
        }
        perror("[DoQ] sendto");
        return -1;
    }
    
    printf("[DoQ] Sent %zd bytes to server\n", sent);
    return sent;
}

/* Extended socket write callback */
static ssize_t doq_write_socket_ex(uint64_t path_id, const unsigned char *buf,
                                   size_t size, const struct sockaddr *peer_addr,
                                   socklen_t peer_addrlen, void *user_data)
{
    return doq_write_socket(buf, size, peer_addr, peer_addrlen, user_data);
}

/* Timer callback - set event timer */
static void doq_set_event_timer(xqc_usec_t wake_after, void *user_data)
{
    doq_client_t *client = (doq_client_t *)user_data;
    
    if (wake_after == 0) {
        /* Immediate wakeup */
        xqc_engine_main_logic(client->engine);
    } else {
        /* Set timer */
        event_loop_set_timer(client->event_loop, wake_after,
                            (timer_callback_t)xqc_engine_main_logic,
                            client->engine);
    }
}

/* Certificate verify callback - accept all certificates */
static int doq_cert_verify(const unsigned char *certs[], const size_t cert_len[],
                           size_t certs_len, void *conn_user_data)
{
    /* For testing purposes, accept all certificates */
    printf("[DoQ] Certificate verification (accepting all)\n");
    return 0;
}

/* Save token callback */
static void doq_save_token(const unsigned char *token, uint32_t token_len,
                           void *user_data)
{
    /* Not implemented for simple test */
}

/* Save session callback */
static void doq_save_session(const char *data, size_t data_len,
                             void *user_data)
{
    /* Not implemented for simple test */
}

/* Save transport params callback */
static void doq_save_tp(const char *data, size_t data_len,
                        void *user_data)
{
    /* Not implemented for simple test */
}

/* Connection create notify */
static int doq_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                                  void *user_data, void *conn_proto_data)
{
    printf("[DoQ] Connection created\n");
    
    /* Set the client context as the ALP user data so stream callbacks can access it */
    doq_client_t *client = (doq_client_t *)user_data;
    xqc_conn_set_alp_user_data(conn, client);
    
    return 0;
}

/* Connection close notify */
static int doq_conn_close_notify(xqc_connection_t *conn, const xqc_cid_t *cid,
                                 void *user_data, void *conn_proto_data)
{
    doq_client_t *client = (doq_client_t *)user_data;
    
    xqc_conn_stats_t stats = xqc_conn_get_stats(client->engine, cid);
    printf("[DoQ] Connection closed: srtt=%"PRIu64"us, conn_err=%d\n",
           stats.srtt, stats.conn_err);
    
    if (!client->response_received) {
        fprintf(stderr, "[DoQ] Connection closed before receiving response\n");
    }
    
    event_loop_stop(client->event_loop);
    
    return 0;
}

/* Handshake finished */
static void doq_conn_handshake_finished(xqc_connection_t *conn, void *user_data,
                                        void *conn_proto_data)
{
    printf("[DoQ] Handshake finished\n");
    
    doq_client_t *client = (doq_client_t *)user_data;
    client->handshake_complete = 1;
    
    /* Create stream and send DNS query */
    doq_send_query(client);
}

/* Stream create notify */
static int doq_stream_create_notify(xqc_stream_t *stream, void *user_data)
{
    printf("[DoQ] Stream created\n");
    return 0;
}

/* Stream write notify - stream is ready to send data */
static int doq_stream_write_notify(xqc_stream_t *stream, void *user_data)
{
    /* We send data immediately after stream creation, so this is not needed */
    return 0;
}

/* Stream read notify - data available to read */
static int doq_stream_read_notify(xqc_stream_t *stream, void *user_data)
{
    doq_client_t *client = (doq_client_t *)user_data;
    ssize_t nread;
    uint8_t buf[PACKET_BUF_SIZE];
    uint8_t fin = 0;
    
    printf("[DoQ] *** Stream read notify called! ***\n");
    
    while (1) {
        nread = xqc_stream_recv(stream, buf, sizeof(buf), &fin);
        
        printf("[DoQ] xqc_stream_recv returned %zd, fin=%d\n", nread, fin);
        
        if (nread == -XQC_EAGAIN) {
            printf("[DoQ] No more data available (EAGAIN)\n");
            break;
        }
        
        if (nread < 0) {
            fprintf(stderr, "[DoQ] Error reading from stream: %zd\n", nread);
            return -1;
        }
        
        printf("[DoQ] Received %zd bytes on stream\n", nread);
        
        /* Append to response buffer */
        if (client->response_len + nread <= sizeof(client->response_buf)) {
            memcpy(client->response_buf + client->response_len, buf, nread);
            client->response_len += nread;
            printf("[DoQ] Total response buffer: %zu bytes\n", client->response_len);
        } else {
            fprintf(stderr, "[DoQ] Response buffer overflow\n");
            return -1;
        }
        
        if (fin) {
            printf("[DoQ] *** Received FIN on stream, complete response! ***\n"); 
            printf("[DoQ] Total DNS response: %zu bytes\n", client->response_len);
            client->response_received = 1;
            doq_process_response(client);
            
            /* Close connection */
            xqc_conn_close(client->engine, &client->cid);
            break;
        }
    }
    
    return 0;
}

/* Stream closing notify - called when stream is being closed/reset */
static void doq_stream_closing_notify(xqc_stream_t *stream, xqc_int_t err_code,
                                      void *user_data)
{
    printf("[DoQ] Stream closing: err_code=%d\n", err_code);
}

/* Stream close notify */
static int doq_stream_close_notify(xqc_stream_t *stream, void *user_data)
{
    printf("[DoQ] Stream closed\n");
    return 0;
}

/*
 * ========================================================================
 * DoQ Client Functions
 * ========================================================================
 */

/* Resolve hostname to IP address */
static int doq_resolve_host(const char *hostname, struct sockaddr_storage *addr,
                            socklen_t *addrlen)
{
    struct addrinfo hints, *res;
    int ret;
    char port_str[16];
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  /* Use IPv4 for better compatibility */
    hints.ai_socktype = SOCK_DGRAM;
    
    snprintf(port_str, sizeof(port_str), "%d", DOQ_PORT);
    
    ret = getaddrinfo(hostname, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    
    memcpy(addr, res->ai_addr, res->ai_addrlen);
    *addrlen = res->ai_addrlen;
    
    /* Print resolved address */
    char ip_str[INET6_ADDRSTRLEN];
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)res->ai_addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
    }
    printf("[DoQ] Resolved %s to %s:%d\n", hostname, ip_str, DOQ_PORT);
    
    freeaddrinfo(res);
    return 0;
}

/* Create UDP socket */
static int doq_create_socket(struct sockaddr_storage *peer_addr)
{
    int fd;
    int flags;
    
    fd = socket(peer_addr->ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    /* Set non-blocking */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl");
        close(fd);
        return -1;
    }
    
    /* Set socket buffer size */
    int bufsize = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    
    return fd;
}

/* Socket read callback for event loop */
static void doq_socket_read_callback(int fd, void *user_data)
{
    doq_client_t *client = (doq_client_t *)user_data;
    uint8_t buf[PACKET_BUF_SIZE];
    ssize_t nread;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = sizeof(peer_addr);
    
    printf("[DoQ] Socket read callback triggered\n");
    
    while (1) {
        nread = recvfrom(fd, buf, sizeof(buf), 0,
                        (struct sockaddr *)&peer_addr, &peer_addrlen);
        
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("recvfrom");
            break;
        }
        
        printf("[DoQ] Received %zd bytes from server\n", nread);
        
        /* Get local address on first packet */
        if (client->local_addrlen == 0) {
            client->local_addrlen = sizeof(client->local_addr);
            getsockname(fd, (struct sockaddr *)&client->local_addr,
                       &client->local_addrlen);
        }
        
        /* Process packet with XQUIC */
        xqc_engine_packet_process(client->engine, buf, nread,
                                 (struct sockaddr *)&client->local_addr,
                                 client->local_addrlen,
                                 (struct sockaddr *)&peer_addr, peer_addrlen,
                                 event_loop_now_us(), client);
    }
    
    /* Trigger engine logic */
    xqc_engine_main_logic(client->engine);
}

/* Build DNS query */
static int doq_build_query(doq_client_t *client)
{
    uint16_t trans_id = (uint16_t)getpid();
    int dns_len;
    
    /* Build DNS query - RFC 9250: DoQ does NOT use length prefix! */
    dns_len = dns_build_query(QUERY_DOMAIN, client->query_buf,
                              sizeof(client->query_buf), trans_id);
    if (dns_len < 0) {
        fprintf(stderr, "Failed to build DNS query\n");
        return -1;
    }
    
    client->query_len = dns_len;
    
    printf("[DoQ] Built DNS query for %s (%zu bytes, no length prefix)\n", 
           QUERY_DOMAIN, client->query_len);
    
    return 0;
}

/* Send DNS query over QUIC stream */
static int doq_send_query(doq_client_t *client)
{
    xqc_stream_settings_t settings = {0};
    ssize_t sent;
    
    /* Create bidirectional stream */
    client->stream = xqc_stream_create(client->engine, &client->cid,
                                       &settings, client);
    if (!client->stream) {
        fprintf(stderr, "[DoQ] Failed to create stream\n");
        return -1;
    }
    
    printf("[DoQ] Created stream, sending DNS query...\n");
    
    /* Debug: print query hex dump */
    printf("[DoQ] Query hex dump: ");
    for (size_t i = 0; i < client->query_len && i < 64; i++) {
        printf("%02x ", client->query_buf[i]);
    }
    printf("\n");
    
    /* RFC 9250: Send DNS query with FIN to close the write side */
    sent = xqc_stream_send(client->stream, client->query_buf,
                          client->query_len, 1);  /* fin=1 */
    if (sent < 0 && sent != -XQC_EAGAIN) {
        fprintf(stderr, "[DoQ] Failed to send query: %zd\n", sent);
        return -1;
    }
    
    if (sent > 0) {
        printf("[DoQ] Sent DNS query (%zd bytes with FIN)\n", sent);
        client->query_sent = 1;
    } else if (sent == -XQC_EAGAIN) {
        printf("[DoQ] Query send blocked (EAGAIN), will retry\n");
    }
    
    /* Trigger engine to send packets */
    xqc_engine_main_logic(client->engine);
    
    return 0;
}

/* Process DNS response */
static void doq_process_response(doq_client_t *client)
{
    uint8_t *dns_msg;
    size_t dns_len;
    char addrs[10][16];
    int addr_count;
    
    /* RFC 9250: DoQ does NOT use length prefix */
    if (client->response_len < 12) {  /* Minimum DNS header size */
        fprintf(stderr, "[DoQ] Response too short: %zu bytes\n", client->response_len);
        return;
    }
    
    dns_msg = client->response_buf;
    dns_len = client->response_len;
    
    printf("[DoQ] Parsing DNS response (%zu bytes, no length prefix)...\n", dns_len);
    
    /* Extract A records */
    addr_count = dns_extract_a_records(dns_msg, dns_len, addrs, 10);
    
    if (addr_count < 0) {
        fprintf(stderr, "[DoQ] Failed to parse DNS response\n");
        return;
    }
    
    printf("\n=== DNS Query Result ===\n");
    printf("Domain: %s\n", QUERY_DOMAIN);
    printf("IPv4 Addresses: %d\n", addr_count);
    
    for (int i = 0; i < addr_count; i++) {
        printf("  %d. %s\n", i + 1, addrs[i]);
    }
    printf("========================\n\n");
}

/* Initialize XQUIC engine */
static int doq_init_engine(doq_client_t *client)
{
    xqc_config_t engine_config;
    xqc_engine_ssl_config_t ssl_config;
    xqc_engine_callback_t callbacks;
    xqc_transport_callbacks_t transport_cbs;
    
    /* Get default config */
    if (xqc_engine_get_default_config(&engine_config, XQC_ENGINE_CLIENT) < 0) {
        fprintf(stderr, "Failed to get default config\n");
        return -1;
    }
    
    /* Set log level */
    engine_config.cfg_log_level = XQC_LOG_WARN;
    
    /* Setup SSL config */
    memset(&ssl_config, 0, sizeof(ssl_config));
    ssl_config.ciphers = XQC_TLS_CIPHERS;
    ssl_config.groups = XQC_TLS_GROUPS;
    
    /* Setup engine callbacks */
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.set_event_timer = doq_set_event_timer;
    
    /* Setup transport callbacks */
    memset(&transport_cbs, 0, sizeof(transport_cbs));
    transport_cbs.write_socket = doq_write_socket;
    transport_cbs.write_socket_ex = doq_write_socket_ex;
    transport_cbs.cert_verify_cb = doq_cert_verify;
    transport_cbs.save_token = doq_save_token;
    transport_cbs.save_session_cb = doq_save_session;
    transport_cbs.save_tp_cb = doq_save_tp;
    
    /* Note: Connection and stream callbacks will be registered via ALPN */
    
    /* Create engine */
    client->engine = xqc_engine_create(XQC_ENGINE_CLIENT, &engine_config,
                                       &ssl_config, &callbacks, &transport_cbs,
                                       client);
    if (!client->engine) {
        fprintf(stderr, "Failed to create XQUIC engine\n");
        return -1;
    }
    
    printf("[DoQ] XQUIC engine created\n");
    
    /* Register DoQ ALPN */
    xqc_conn_callbacks_t conn_cbs;
    memset(&conn_cbs, 0, sizeof(conn_cbs));
    conn_cbs.conn_create_notify = doq_conn_create_notify;
    conn_cbs.conn_close_notify = doq_conn_close_notify;
    conn_cbs.conn_handshake_finished = doq_conn_handshake_finished;
    
    xqc_stream_callbacks_t stream_cbs;
    memset(&stream_cbs, 0, sizeof(stream_cbs));
    stream_cbs.stream_create_notify = doq_stream_create_notify;
    stream_cbs.stream_write_notify = doq_stream_write_notify;
    stream_cbs.stream_read_notify = doq_stream_read_notify;
    stream_cbs.stream_close_notify = doq_stream_close_notify;
    stream_cbs.stream_closing_notify = doq_stream_closing_notify;
    
    printf("[DoQ] Registering stream callbacks:\n");
    printf("  - stream_create_notify: %p\n", (void*)stream_cbs.stream_create_notify);
    printf("  - stream_read_notify: %p\n", (void*)stream_cbs.stream_read_notify);
    printf("  - stream_write_notify: %p\n", (void*)stream_cbs.stream_write_notify);
    printf("  - stream_close_notify: %p\n", (void*)stream_cbs.stream_close_notify);
    printf("  - stream_closing_notify: %p\n", (void*)stream_cbs.stream_closing_notify);
    
    xqc_app_proto_callbacks_t app_proto_cbs;
    app_proto_cbs.conn_cbs = conn_cbs;
    app_proto_cbs.stream_cbs = stream_cbs;
    
    if (xqc_engine_register_alpn(client->engine, DOQ_ALPN, strlen(DOQ_ALPN),
                                 &app_proto_cbs, client) != 0) {
        fprintf(stderr, "Failed to register DoQ ALPN\n");
        xqc_engine_destroy(client->engine);
        client->engine = NULL;
        return -1;
    }
    
    printf("[DoQ] Registered ALPN: %s\n", DOQ_ALPN);
    return 0;
}

/* Create QUIC connection */
static int doq_create_connection(doq_client_t *client)
{
    xqc_conn_settings_t conn_settings;
    xqc_conn_ssl_config_t ssl_config;
    const xqc_cid_t *cid;
    
    /* Setup connection settings */
    memset(&conn_settings, 0, sizeof(conn_settings));
    conn_settings.pacing_on = 1;
    conn_settings.cong_ctrl_callback = xqc_bbr_cb;
    conn_settings.cc_params.customize_on = 0;
    
    /* Setup SSL config */
    memset(&ssl_config, 0, sizeof(ssl_config));
    
    /* Create connection */
    cid = xqc_connect(client->engine, &conn_settings,
                     NULL, 0,  /* no token */
                     DOQ_SERVER, 0,  /* no_crypto = 0 */
                     &ssl_config,
                     (struct sockaddr *)&client->peer_addr,
                     client->peer_addrlen,
                     DOQ_ALPN, client);
    
    if (!cid) {
        fprintf(stderr, "Failed to create QUIC connection\n");
        return -1;
    }
    
    memcpy(&client->cid, cid, sizeof(xqc_cid_t));
    printf("[DoQ] QUIC connection initiated\n");
    
    /* Trigger initial packet send */
    xqc_engine_main_logic(client->engine);
    
    return 0;
}

/* Main function */
int main(int argc, char *argv[])
{
    doq_client_t client;
    int ret = 0;
    
    memset(&client, 0, sizeof(client));
    g_client = &client;
    
    printf("=== DNS-over-QUIC Client ===\n");
    printf("Server: %s:%d\n", DOQ_SERVER, DOQ_PORT);
    printf("Query: %s (A record)\n\n", QUERY_DOMAIN);
    
    /* Resolve server address */
    if (doq_resolve_host(DOQ_SERVER, &client.peer_addr, &client.peer_addrlen) < 0) {
        return 1;
    }
    
    /* Create socket */
    client.sockfd = doq_create_socket(&client.peer_addr);
    if (client.sockfd < 0) {
        return 1;
    }
    
    /* Create event loop */
    client.event_loop = event_loop_create();
    if (!client.event_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        close(client.sockfd);
        return 1;
    }
    
    /* Add socket to event loop */
    event_loop_add_socket(client.event_loop, client.sockfd,
                         doq_socket_read_callback, &client);
    
    /* Build DNS query */
    if (doq_build_query(&client) < 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* Initialize XQUIC engine */
    if (doq_init_engine(&client) < 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* Create QUIC connection */
    if (doq_create_connection(&client) < 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* Run event loop */
    printf("[DoQ] Starting event loop...\n\n");
    event_loop_run(client.event_loop);
    
    printf("[DoQ] Event loop stopped\n");
    
cleanup:
    /* Cleanup */
    if (client.engine) {
        xqc_engine_destroy(client.engine);
    }
    
    if (client.event_loop) {
        event_loop_destroy(client.event_loop);
    }
    
    if (client.sockfd >= 0) {
        close(client.sockfd);
    }
    
    return ret;
}


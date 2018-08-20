#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#if defined(__OpenBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#include <libwebsockets.h>
#include <json.h>

#include "server.h"
#include "utils.h"

// initial message list
char initial_cmds[] = {
        SET_WINDOW_TITLE,
        SET_RECONNECT,
        SET_PREFERENCES
};

int
send_initial_message(struct lws *wsi, int index) {
    unsigned char message[LWS_PRE + 1 + 4096];
    unsigned char *p = &message[LWS_PRE];
    char buffer[128];
    int n = 0;

    char cmd = initial_cmds[index];
    switch(cmd) {
        case SET_WINDOW_TITLE:
            gethostname(buffer, sizeof(buffer) - 1);
            n = sprintf((char *) p, "%c%s (%s)", cmd, server->command, buffer);
            break;
        case SET_RECONNECT:
            n = sprintf((char *) p, "%c%d", cmd, server->reconnect);
            break;
        case SET_PREFERENCES:
            n = sprintf((char *) p, "%c%s", cmd, server->prefs_json);
            break;
        default:
            break;
    }

    return lws_write(wsi, p, (size_t) n, LWS_WRITE_BINARY);
}

bool
parse_window_size(const char *json, struct winsize *size) {
    int columns, rows;
    json_object *obj = json_tokener_parse(json);
    struct json_object *o = NULL;

    if (!json_object_object_get_ex(obj, "columns", &o)) {
        lwsl_err("columns field not exists, json: %s\n", json);
        return false;
    }
    columns = json_object_get_int(o);
    if (!json_object_object_get_ex(obj, "rows", &o)) {
        lwsl_err("rows field not exists, json: %s\n", json);
        return false;
    }
    rows = json_object_get_int(o);
    json_object_put(obj);

    memset(size, 0, sizeof(struct winsize));
    size->ws_col = (unsigned short) columns;
    size->ws_row = (unsigned short) rows;

    return true;
}

bool
check_host_origin(struct lws *wsi) {
    int origin_length = lws_hdr_total_length(wsi, WSI_TOKEN_ORIGIN);
    char buf[origin_length + 1];
    memset(buf, 0, sizeof(buf));
    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_ORIGIN);
    if (len <= 0) {
        return false;
    }

    const char *prot, *address, *path;
    int port;
    if (lws_parse_uri(buf, &prot, &address, &port, &path))
        return false;
    if (port == 80 || port == 443) {
        sprintf(buf, "%s", address);
    } else {
        sprintf(buf, "%s:%d", address, port);
    }

    int host_length = lws_hdr_total_length(wsi, WSI_TOKEN_HOST);
    if (host_length != strlen(buf))
        return false;
    char host_buf[host_length + 1];
    memset(host_buf, 0, sizeof(host_buf));
    len = lws_hdr_copy(wsi, host_buf, sizeof(host_buf), WSI_TOKEN_HOST);

    return len > 0 && strcasecmp(buf, host_buf) == 0;
}

void
tty_client_remove(struct tty_client *client) {
    uv_mutex_lock(&server->mutex);
    struct tty_client *iterator;
    LIST_FOREACH(iterator, &server->clients, list) {
        if (iterator == client) {
            LIST_REMOVE(iterator, list);
            server->client_count--;
            break;
        }
    }
    uv_mutex_unlock(&server->mutex);
}

void
tty_client_destroy(struct tty_client *client) {
    if (!client->running || client->pid <= 0)
        goto cleanup;

    client->running = false;

    // kill process and free resource
    lwsl_notice("sending %s (%d) to process %d\n", server->sig_name, server->sig_code, client->pid);
    if (kill(client->pid, server->sig_code) != 0) {
        lwsl_err("kill: %d, errno: %d (%s)\n", client->pid, errno, strerror(errno));
    }
    int status;
    while (waitpid(client->pid, &status, 0) == -1 && errno == EINTR)
        ;
    lwsl_notice("process exited with code %d, pid: %d\n", status, client->pid);
    close(client->pty);

cleanup:
    // free the buffer
    if (client->buffer != NULL)
        free(client->buffer);
    if (client->pty_buffer != NULL)
        free(client->pty_buffer);

    uv_stop(client->loop);

    uv_mutex_destroy(&client->mutex);
    uv_cond_destroy(&client->cond);
#if UV_VERSION_MAJOR == 0
    uv_loop_delete(client->loop);
#else
    uv_loop_close(client->loop);
    free(client->loop);
#endif

    // remove from client list
    tty_client_remove(client);
}

#if UV_VERSION_MAJOR == 0
uv_buf_t
alloc_cb(uv_handle_t* handle, size_t suggested_size) {
    return uv_buf_init(malloc(suggested_size), suggested_size);
}
#else
void
alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = xmalloc(suggested_size);
    buf->len = suggested_size;
}
#endif

void
#if UV_VERSION_MAJOR == 0
read_cb(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {
#else
read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
#endif
    struct tty_client *client = (struct tty_client *) stream->data;
    while (client->running) {
        uv_mutex_lock(&client->mutex);
        if (client->state == STATE_READY) {
            uv_cond_wait(&client->cond, &client->mutex);
        }
        if (nread <= 0) {
            if (nread == UV_ENOBUFS || nread == 0) {
                continue;
            }
            client->pty_len = 0;
            client->pty_buffer = NULL;
            if (nread != UV_EOF) {
#if UV_VERSION_MAJOR == 0
                uv_err_t err = uv_last_error(client->loop);
                lwsl_err("[%s] closing stream: %s (%s)\n", client->hostname, uv_err_name(err), strerror((int)nread));
#else
                lwsl_err("[%s] closing stream: %s (%s)\n", client->hostname, uv_err_name((int)nread), strerror((int)nread));
#endif
                uv_read_stop(stream);
            }
        } else {
            client->pty_len = nread;
            client->pty_buffer = xmalloc(LWS_PRE + 1 + (size_t ) nread);
#if UV_VERSION_MAJOR == 0
            memcpy(client->pty_buffer + LWS_PRE + 1, buf.base, nread);
        }
        free(buf.base);
#else
            memcpy(client->pty_buffer + LWS_PRE + 1, buf->base, nread);
        }
        free(buf->base);
#endif
        client->state = STATE_READY;
        uv_mutex_unlock(&client->mutex);
        break;
    }
}

void
thread_cb(void *args) {
    int pty;
    struct tty_client *client = (struct tty_client *) args;
    pid_t pid = forkpty(&pty, NULL, NULL, NULL);
    if (pid == -1) {
        lwsl_err("forkpty, error: %d (%s)\n", errno, strerror(errno));
        return;
    } else if (pid == 0) {
        setsid();
        setenv("TERM", "xterm-256color", true);
        if (execvp(server->argv[0], server->argv) < 0) {
            fprintf(stderr, "execvp error: %s", strerror(errno));
        }
    }

    lwsl_notice("started process, pid: %d\n", pid);
    client->pid = pid;
    client->pty = pty;
    client->running = true;
    if (client->size.ws_row > 0 && client->size.ws_col > 0)
        ioctl(client->pty, TIOCSWINSZ, &client->size);

    uv_pipe_init(client->loop, &client->pipe, 0);
    client->pipe.data = client;
    uv_pipe_open(&client->pipe, pty);
    uv_read_start((uv_stream_t *)& client->pipe, alloc_cb, read_cb);

    uv_run(client->loop, 0);
}

int
callback_tty(struct lws *wsi, enum lws_callback_reasons reason,
             void *user, void *in, size_t len) {
    struct tty_client *client = (struct tty_client *) user;
    char buf[256];
    size_t n = 0;

    switch (reason) {
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            if (server->once && server->client_count > 0) {
                lwsl_warn("refuse to serve WS client due to the --once option.\n");
                return 1;
            }
            if (server->max_clients > 0 && server->client_count == server->max_clients) {
                lwsl_warn("refuse to serve WS client due to the --max-clients option.\n");
                return 1;
            }
            if (lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI) <= 0 || strcmp(buf, WS_PATH) != 0) {
                lwsl_warn("refuse to serve WS client for illegal ws path: %s\n", buf);
                return 1;
            }

            if (server->check_origin && !check_host_origin(wsi)) {
                lwsl_warn("refuse to serve WS client from different origin due to the --check-origin option.\n");
                return 1;
            }
            break;

        case LWS_CALLBACK_ESTABLISHED:
            client->running = false;
            client->initialized = false;
            client->initial_cmd_index = 0;
            client->authenticated = false;
            client->wsi = wsi;
            client->buffer = NULL;
            client->state = STATE_INIT;
            client->pty_len = 0;
            uv_mutex_init(&client->mutex);
            uv_cond_init(&client->cond);
#if UV_VERSION_MAJOR == 0
            client->loop = uv_loop_new();
#else
            client->loop = xmalloc(sizeof *client->loop);
            uv_loop_init(client->loop);
#endif
            lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi),
                                   client->hostname, sizeof(client->hostname),
                                   client->address, sizeof(client->address));

            uv_mutex_lock(&server->mutex);
            LIST_INSERT_HEAD(&server->clients, client, list);
            server->client_count++;
            uv_mutex_unlock(&server->mutex);
            lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI);

            lwsl_notice("WS   %s - %s (%s), clients: %d\n", buf, client->address, client->hostname, server->client_count);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (!client->initialized) {
                if (client->initial_cmd_index == sizeof(initial_cmds)) {
                    client->initialized = true;
                    break;
                }
                if (send_initial_message(wsi, client->initial_cmd_index) < 0) {
                    tty_client_remove(client);
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                    return -1;
                }
                client->initial_cmd_index++;
                lws_callback_on_writable(wsi);
                return 0;
            }
            if (client->state != STATE_READY)
                break;

            // read error or client exited, close connection
            if (client->pty_len <= 0) {
                tty_client_remove(client);
                lws_close_reason(wsi,
                                 client->pty_len == 0 ? LWS_CLOSE_STATUS_NORMAL
                                                       : LWS_CLOSE_STATUS_UNEXPECTED_CONDITION,
                                 NULL, 0);
                return -1;
            }

            client->pty_buffer[LWS_PRE] = OUTPUT;
            n = (size_t) (client->pty_len + 1);
            if (lws_write(wsi, (unsigned char *) client->pty_buffer + LWS_PRE, n, LWS_WRITE_BINARY) < n) {
                lwsl_err("write data to WS\n");
            }
            free(client->pty_buffer);
            client->pty_buffer = NULL;
            client->state = STATE_DONE;
            break;

        case LWS_CALLBACK_RECEIVE:
            if (client->buffer == NULL) {
                client->buffer = xmalloc(len);
                client->len = len;
                memcpy(client->buffer, in, len);
            } else {
                client->buffer = xrealloc(client->buffer, client->len + len);
                memcpy(client->buffer + client->len, in, len);
                client->len += len;
            }

            const char command = client->buffer[0];

            // check auth
            if (server->credential != NULL && !client->authenticated && command != JSON_DATA) {
                lwsl_warn("WS client not authenticated\n");
                return 1;
            }

            // check if there are more fragmented messages
            if (lws_remaining_packet_payload(wsi) > 0 || !lws_is_final_fragment(wsi)) {
                return 0;
            }

            switch (command) {
                case INPUT:
                    if (client->pty == 0)
                        break;
                    if (server->readonly)
                        return 0;
                    if (write(client->pty, client->buffer + 1, client->len - 1) == -1) {
                        lwsl_err("write INPUT to pty: %d (%s)\n", errno, strerror(errno));
                        tty_client_remove(client);
                        lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                        return -1;
                    }
                    break;
                case RESIZE_TERMINAL:
                    if (parse_window_size(client->buffer + 1, &client->size) && client->pty > 0) {
                        if (ioctl(client->pty, TIOCSWINSZ, &client->size) == -1) {
                            lwsl_err("ioctl TIOCSWINSZ: %d (%s)\n", errno, strerror(errno));
                        }
                    }
                    break;
                case JSON_DATA:
                    if (client->pid > 0)
                        break;
                    if (server->credential != NULL) {
                        json_object *obj = json_tokener_parse(client->buffer);
                        struct json_object *o = NULL;
                        if (json_object_object_get_ex(obj, "AuthToken", &o)) {
                            const char *token = json_object_get_string(o);
                            if (token != NULL && !strcmp(token, server->credential))
                                client->authenticated = true;
                            else
                                lwsl_warn("WS authentication failed with token: %s\n", token);
                        }
                        if (!client->authenticated) {
                            tty_client_remove(client);
                            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
                            return -1;
                        }
                    }
                    int err = uv_thread_create(&client->thread, thread_cb, client);
                    if (err != 0) {
                        lwsl_err("uv_thread_create return: %d\n", err);
                        return 1;
                    }
                    break;
                default:
                    lwsl_warn("ignored unknown message type: %c\n", command);
                    break;
            }

            if (client->buffer != NULL) {
                free(client->buffer);
                client->buffer = NULL;
            }
            break;

        case LWS_CALLBACK_CLOSED:
            tty_client_destroy(client);
            lwsl_notice("WS closed from %s (%s), clients: %d\n", client->address, client->hostname, server->client_count);
            if (server->once && server->client_count == 0) {
                lwsl_notice("exiting due to the --once option.\n");
                force_exit = true;
                lws_cancel_service(context);
                exit(0);
            }
            break;

        default:
            break;
    }

    return 0;
}
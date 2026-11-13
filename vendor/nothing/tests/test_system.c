#include "test.h"

#include "nb/encoding.h"
#include "nb/file.h"
#include "nb/ini.h"
#include "nb/socket.h"
#include "nb/thread.h"

#include <stdatomic.h>

static void test_encoding(void)
{
    nb_buf *out = nb_buf_new();
    nb_err err;
    nb_err_clear(&err);

    NB_CHECK_INT(nb_base64_encode("hello", 5, NB_B64_STD, true, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "aGVsbG8=");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_base64_decode(NB_STR_LIT("aGVsbG8="), NB_B64_STD, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "hello");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_base64_encode("hello", 5, NB_B64_URL, false, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "aGVsbG8");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_base64_decode(NB_STR_LIT("aGVsbG8"), NB_B64_URL, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "hello");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_base64_decode(NB_STR_LIT("aGVsbG8=extra"), NB_B64_STD, out, &err),
                 NB_ERR_PARSE);
    NB_CHECK_INT(nb_buf_len(out), 0);

    nb_buf_clear(out);
    NB_CHECK_INT(nb_hex_encode("\x01\xab", 2, true, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "01AB");
    char hex[5];
    NB_CHECK_INT(nb_hex_encode_to("\x01\xab", 2, true, hex, sizeof(hex),
                                  &err),
                 NB_OK);
    NB_CHECK_STR(hex, "01AB");
    char too_small[4];
    NB_CHECK_INT(nb_hex_encode_to("\x01\xab", 2, true, too_small,
                                  sizeof(too_small), &err),
                 NB_ERR_RANGE);
    nb_buf_clear(out);
    NB_CHECK_INT(nb_hex_decode(NB_STR_LIT("01ab"), out, &err), NB_OK);
    NB_CHECK_INT(nb_buf_len(out), 2);
    NB_CHECK_INT((unsigned char)out->ptr[0], 1);
    NB_CHECK_INT((unsigned char)out->ptr[1], 0xab);

    nb_buf_clear(out);
    NB_CHECK_INT(nb_url_encode(NB_STR_LIT("a b/c?d=e"), true, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "a+b%2Fc%3Fd%3De");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_url_decode(NB_STR_LIT("a+b%2Fc%3Fd%3De"), true, out, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(out), "a b/c?d=e");
    nb_buf_clear(out);
    NB_CHECK_INT(nb_url_decode(NB_STR_LIT("%zz"), false, out, &err), NB_ERR_PARSE);
    NB_CHECK_INT(nb_buf_len(out), 0);

    nb_buf_free(out);
}

static void test_ini(void)
{
    const char *text =
        "# comment\n"
        "name = nbase\n"
        "\n"
        "[server]\n"
        "host = 127.0.0.1\n"
        "port = 8080\n"
        "enabled = true\n"
        "quoted = \"hello world\"\n";
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(text), &err);
    NB_CHECK(ini != NULL);
    if (!ini) {
        return;
    }
    NB_CHECK_STR(nb_ini_get(ini, "", "name"), "nbase");
    nb_str name = nb_ini_get_str(ini, "", "name");
    NB_CHECK(name.ptr != NULL && name.len == 5u);
    if (name.ptr && name.len == 5u) {
        NB_CHECK_MEM(name.ptr, "nbase", 5u);
    }
    NB_CHECK(nb_ini_get_str(ini, "", "missing").ptr == NULL);
    NB_CHECK_STR(nb_ini_get(ini, "server", "host"), "127.0.0.1");
    NB_CHECK_STR(nb_ini_get(ini, "server", "quoted"), "hello world");
    NB_CHECK(nb_ini_get_bool(ini, "server", "enabled", false));
    NB_CHECK_INT(nb_ini_get_int(ini, "server", "port", 0), 8080);
    NB_CHECK_INT(nb_ini_section_count(ini), 2);
    NB_CHECK_INT(nb_ini_count(ini, "server"), 4);
    NB_CHECK_INT(nb_ini_set(ini, "server", "port", "9090"), NB_OK);
    NB_CHECK_STR(nb_ini_get(ini, "server", "port"), "9090");
    NB_CHECK(nb_ini_remove(ini, "server", "quoted"));
    NB_CHECK(!nb_ini_has(ini, "server", "quoted"));

    nb_buf *out = nb_buf_new();
    NB_CHECK_INT(nb_ini_write(ini, out), NB_OK);
    NB_CHECK(strstr(nb_buf_cstr(out), "[server]") != NULL);
    NB_CHECK(strstr(nb_buf_cstr(out), "port = 9090") != NULL);
    nb_buf_free(out);

    nb_ini *bad = nb_ini_from_string(NB_STR_LIT("[broken\n"), &err);
    NB_CHECK(bad == NULL);
    NB_CHECK_INT(err.code, NB_ERR_PARSE);

    nb_ini_iter it;
    nb_ini_iter_init(ini, &it);
    const char *section;
    const char *key;
    const char *value;
    int count = 0;
    while (nb_ini_iter_next(&it, &section, &key, &value)) {
        NB_CHECK(section != NULL && key != NULL && value != NULL);
        count++;
    }
    NB_CHECK_INT(count, 4);
    nb_ini_free(ini);
}

static void test_file(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *path = nb_buf_new();
    NB_CHECK_INT(nb_file_temp(path, &err), NB_OK);
    if (err.code != NB_OK) {
        nb_buf_free(path);
        return;
    }
    NB_CHECK_INT(nb_file_write_all(nb_buf_cstr(path), "hello", 5, &err), NB_OK);
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));
    uint64_t size = 0;
    NB_CHECK_INT(nb_file_size(nb_buf_cstr(path), &size, &err), NB_OK);
    NB_CHECK_INT(size, 5);

    nb_buf *data = nb_buf_new();
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(path), data, 0, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(data), "hello");
    NB_CHECK_INT(nb_file_append(nb_buf_cstr(path), " world", 6, &err), NB_OK);
    NB_CHECK_INT(nb_file_read_all(nb_buf_cstr(path), data, 0, &err), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(data), "hello world");

    NB_CHECK_STR(nb_path_basename("/a/b/c.txt"), "c.txt");
    NB_CHECK_STR(nb_path_extension("/a/b/c.txt"), "txt");
    NB_CHECK(nb_path_extension("/a/b/.hidden") == NULL);

    nb_buf *joined = nb_buf_new();
    NB_CHECK_INT(nb_path_join(joined, "/a/b", "c"), NB_OK);
    NB_CHECK_STR(nb_buf_cstr(joined), "/a/b/c");
    NB_CHECK_INT(nb_file_remove(nb_buf_cstr(path), &err), NB_OK);
    nb_buf_free(joined);

    nb_buf *dir_path = nb_buf_new();
    NB_CHECK_INT(nb_buf_append_cstr(dir_path, nb_buf_cstr(path)), true);
    NB_CHECK(nb_buf_append_cstr(dir_path, ".dir/a/b"));
    NB_CHECK_INT(nb_file_mkdir(nb_buf_cstr(dir_path), true, &err), NB_OK);
    NB_CHECK(nb_file_is_dir(nb_buf_cstr(dir_path)));
    nb_dir *dir = nb_dir_open(nb_buf_cstr(dir_path), &err);
    NB_CHECK(dir != NULL);
    if (dir) {
        (void)nb_dir_next(dir, NULL);
        NB_CHECK_INT(nb_dir_status(dir), NB_OK);
        nb_dir_close(dir);
    }
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(dir_path), &err), NB_OK);

    nb_buf *ancestor = nb_buf_new();
    NB_CHECK(nb_buf_append_cstr(ancestor, nb_buf_cstr(path)));
    NB_CHECK(nb_buf_append_cstr(ancestor, ".dir/a"));
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(ancestor), &err), NB_OK);
    nb_buf_clear(ancestor);
    NB_CHECK(nb_buf_append_cstr(ancestor, nb_buf_cstr(path)));
    NB_CHECK(nb_buf_append_cstr(ancestor, ".dir"));
    NB_CHECK_INT(nb_file_rmdir(nb_buf_cstr(ancestor), &err), NB_OK);
    nb_buf_free(ancestor);

    nb_dir *parent = nb_dir_open(nb_buf_cstr(path), &err);
    NB_CHECK(parent == NULL);
    (void)parent;
    nb_buf_free(dir_path);
    nb_buf_free(data);
    nb_buf_free(path);
}

typedef struct thread_state {
    nb_atomic_int counter;
} thread_state;

static void *increment_worker(void *arg)
{
    thread_state *state = arg;
    for (int i = 0; i < 10000; i++) {
        nb_atomic_int_fetch_add(&state->counter, 1);
    }
    return NULL;
}

static void test_thread(void)
{
    thread_state state;
    nb_atomic_int_init(&state.counter, 0);
    nb_thread *threads[4];
    for (size_t i = 0; i < NB_ARRAY_SIZE(threads); i++) {
        threads[i] = NULL;
        NB_CHECK_INT(nb_thread_create(&threads[i], increment_worker, &state, NULL), NB_OK);
    }
    for (size_t i = 0; i < NB_ARRAY_SIZE(threads); i++) {
        if (threads[i]) {
            NB_CHECK_INT(nb_thread_join(threads[i], NULL), NB_OK);
            nb_thread_free(threads[i]);
        }
    }
    NB_CHECK_INT(nb_atomic_int_load(&state.counter), 40000);

    nb_mutex *mutex = nb_mutex_new(0);
    nb_cond *cond = nb_cond_new();
    NB_CHECK(mutex != NULL && cond != NULL);
    if (mutex && cond) {
        nb_mutex_lock(mutex);
        NB_CHECK_INT(nb_cond_timedwait(cond, mutex, 1000000), NB_ERR_TIMEOUT);
        nb_mutex_unlock(mutex);
    }
    nb_cond_free(cond);
    nb_mutex_free(mutex);
}

typedef struct socket_server_state {
    nb_socket *server;
    nb_status status;
} socket_server_state;

static void *socket_server_worker(void *arg)
{
    socket_server_state *state = arg;
    nb_socket *client = NULL;
    nb_err err;
    nb_err_clear(&err);
    state->status = nb_socket_accept(state->server, &client, NULL, &err);
    if (state->status != NB_OK) {
        return NULL;
    }
    char request[6] = { 0 };
    state->status = nb_socket_recv_all(client, request, 5, &err);
    if (state->status == NB_OK) {
        state->status = nb_socket_send_all(client, "world", 5, &err);
    }
    nb_socket_free(client);
    return NULL;
}

static void test_socket(void)
{
    nb_err err;
    nb_err_clear(&err);

    nb_addr addr;
    NB_CHECK_INT(nb_addr_parse("127.0.0.1", 8080, &addr, &err), NB_OK);
    char text[64];
    NB_CHECK_INT(nb_addr_format(&addr, text, sizeof(text), true), NB_OK);
    NB_CHECK_STR(text, "127.0.0.1:8080");
    nb_addr copy;
    NB_CHECK_INT(nb_addr_parse("127.0.0.1", 8080, &copy, &err), NB_OK);
    NB_CHECK(nb_addr_equal(&addr, &copy));

    nb_socket *server = NULL;
    NB_CHECK_INT(nb_socket_open(&server, NB_AF_INET, NB_SOCK_STREAM, &err), NB_OK);
    if (!server) {
        return;
    }
    NB_CHECK_INT(nb_socket_set_reuseaddr(server, true, &err), NB_OK);
    nb_addr bind_addr;
    NB_CHECK_INT(nb_addr_parse("127.0.0.1", 0, &bind_addr, &err), NB_OK);
    NB_CHECK_INT(nb_socket_bind(server, &bind_addr, &err), NB_OK);
    NB_CHECK_INT(nb_socket_listen(server, 8, &err), NB_OK);
    nb_addr local;
    NB_CHECK_INT(nb_socket_local_addr(server, &local, &err), NB_OK);
    NB_CHECK(local.port != 0);

    socket_server_state server_state;
    server_state.server = server;
    server_state.status = NB_ERR_STATE;
    nb_thread *thread = NULL;
    NB_CHECK_INT(nb_thread_create(&thread, socket_server_worker, &server_state, NULL), NB_OK);

    nb_socket *client = NULL;
    NB_CHECK_INT(nb_socket_open(&client, NB_AF_INET, NB_SOCK_STREAM, &err), NB_OK);
    if (client) {
        NB_CHECK_INT(nb_socket_connect(client, &local, 2000, &err), NB_OK);
        NB_CHECK_INT(nb_socket_send_all(client, "hello", 5, &err), NB_OK);
        char reply[6] = { 0 };
        NB_CHECK_INT(nb_socket_recv_all(client, reply, 5, &err), NB_OK);
        NB_CHECK_STR(reply, "world");
        nb_socket_free(client);
    }
    if (thread) {
        NB_CHECK_INT(nb_thread_join(thread, NULL), NB_OK);
        nb_thread_free(thread);
    }
    NB_CHECK_INT(server_state.status, NB_OK);
    nb_socket_free(server);
}

void test_system(void)
{
    test_encoding();
    test_ini();
    test_file();
    test_thread();
    test_socket();
}

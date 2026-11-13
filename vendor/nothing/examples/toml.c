#include "nb/nbase.h"

#include <stdio.h>

static void print_string(const nb_toml_value *value, const char *label)
{
    nb_str text;
    if (nb_toml_get_string(value, &text)) {
        printf("%s=%.*s\n", label, (int)text.len, text.ptr ? text.ptr : "");
    }
}

int main(void)
{
    const char *source =
        "title = \"nbase config\"\n"
        "enabled = true\n"
        "\n"
        "[server]\n"
        "host = \"127.0.0.1\"\n"
        "port = 8080\n"
        "tags = [\"api\", \"edge\"]\n"
        "\n"
        "[server.limits]\n"
        "retries = 3\n"
        "ratio = 0.75\n"
        "\n"
        "[[routes]]\n"
        "path = \"/health\"\n"
        "\n"
        "[[routes]]\n"
        "path = \"/metrics\"\n"
        "started = 2026-11-11T09:30:00Z\n";

    nb_err err;
    nb_err_clear(&err);
    nb_toml *doc = nb_toml_parse(nb_str_from_cstr(source), &err);
    if (!doc) {
        printf("parse failed: %s\n", err.msg);
        return 1;
    }

    const nb_toml_value *root = nb_toml_root(doc);
    print_string(nb_toml_table_get_cstr(root, "title"), "title");

    bool enabled = false;
    if (nb_toml_get_bool(nb_toml_table_get_cstr(root, "enabled"), &enabled)) {
        printf("enabled=%s\n", enabled ? "true" : "false");
    }

    const nb_toml_value *server =
        nb_toml_table_get_cstr(root, "server");
    print_string(nb_toml_table_get_cstr(server, "host"), "host");

    int64_t port = 0;
    if (nb_toml_get_integer(nb_toml_table_get_cstr(server, "port"), &port)) {
        printf("port=%lld\n", (long long)port);
    }

    const nb_toml_value *tags =
        nb_toml_table_get_cstr(server, "tags");
    printf("tags=%zu\n", nb_toml_array_size(tags));
    for (size_t i = 0; i < nb_toml_array_size(tags); i++) {
        print_string(nb_toml_array_get(tags, i), "  tag");
    }

    const nb_toml_value *limits =
        nb_toml_table_get_cstr(server, "limits");
    double ratio = 0.0;
    if (nb_toml_get_float(nb_toml_table_get_cstr(limits, "ratio"), &ratio)) {
        printf("ratio=%.2f\n", ratio);
    }

    const nb_toml_value *routes =
        nb_toml_table_get_cstr(root, "routes");
    printf("routes=%zu\n", nb_toml_array_size(routes));
    for (size_t i = 0; i < nb_toml_array_size(routes); i++) {
        const nb_toml_value *route = nb_toml_array_get(routes, i);
        print_string(nb_toml_table_get_cstr(route, "path"), "  route");
    }

    const nb_toml_value *last_route =
        nb_toml_array_get(routes, nb_toml_array_size(routes) - 1u);
    nb_toml_datetime started;
    if (nb_toml_get_datetime(
            nb_toml_table_get_cstr(last_route, "started"), &started)) {
        printf("started=%04d-%02d-%02dT%02d:%02d:%02dZ\n",
               (int)started.year, (int)started.month, (int)started.day,
               (int)started.hour, (int)started.minute,
               (int)started.second);
    }

    printf("--- root keys ---\n");
    nb_toml_table_iter iter;
    nb_toml_table_iter_init(root, &iter);
    nb_str key;
    const nb_toml_value *value;
    while (nb_toml_table_iter_next(&iter, &key, &value)) {
        printf("%.*s\n", (int)key.len, key.ptr);
    }

    nb_toml_free(doc);
    return 0;
}

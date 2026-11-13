#include "nb/nbase.h"

#include <stdio.h>

enum {
    MY_ERR_CONFIG = NB_ERR_USER(1),
    MY_ERR_AUTH = NB_ERR_USER(2)
};

static const nb_err_desc my_errors[] = {
    { MY_ERR_CONFIG, "MY_ERR_CONFIG", "config invalid" },
    { MY_ERR_AUTH, "MY_ERR_AUTH", "authentication failed" },
};

int main(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_err_set(&err, NB_ERR_IO, "cannot open '%s'", "config.ini");
    printf("code=%d (%s)\n", (int)err.code, nb_status_str(err.code));
    printf("message=%s\n", err.msg);

    nb_err_set_os(&err, NB_ERR_OS, 2, "stat failed");
    printf("os_errno=%d\n", err.os_errno);

    nb_err_set(&err, MY_ERR_CONFIG, "cannot open '%s'", "config.ini");
    const nb_err_desc *desc =
        nb_err_lookup(my_errors, NB_ARRAY_SIZE(my_errors), err.code);
    printf("user code=%d name=%s text=%s\n", err.code,
           desc ? desc->name : "(unknown)",
           nb_err_str(my_errors, NB_ARRAY_SIZE(my_errors), err.code));
    printf("os_errno reset=%d\n", err.os_errno);
    return 0;
}

#ifndef MORPH_ORM_FCGI_STORE_H
#define MORPH_ORM_FCGI_STORE_H

#include "persistence/orm/database.h"

struct session_store;

int orm_fcgi_schema(struct db *db);
/* A transaction retains the database lock until its matching end. */
int fcgi_store_transaction_begin(struct session_store *store);
int fcgi_store_transaction_end(struct session_store *store, int result);

#endif

#include <gtest/gtest.h>
#include "config/config.h"
#include "persistence/orm/database.h"
#include "persistence/orm/session.h"
#include "persistence/orm/unit_of_work.h"
#include "sapi/fastcgi/session_store.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/log.h>
#include <nb/time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fstream>
#include <string>
#include <unistd.h>

struct ConnectionTestFile {
	char path[PATH_MAX] = "/tmp/morph-db-connection-XXXXXX";
	ConnectionTestFile()
	{
		int fd = mkstemp(path);
		if (fd >= 0)
			close(fd);
	}
	~ConnectionTestFile()
	{
		std::remove(path);
		std::remove((std::string(path) + "-wal").c_str());
		std::remove((std::string(path) + "-shm").c_str());
	}
};

static void connection_store_cleanup(struct session_store **owner)
{
	session_store_close(*owner);
	*owner = nullptr;
}

TEST(DatabaseConnectionTest, DefaultConfigurationOpensSQLiteFastcgiStorage)
{
	ConnectionTestFile file;
	auto config = std::make_unique<struct config>();
	struct db_options options = {};

	config_set_defaults(nullptr, config.get());
	ASSERT_EQ(config_database_options(&config->database, file.path, &options), NB_OK);
	EXPECT_EQ(options.tls_mode, DB_TLS_DEFAULT);
	struct session_store *store NB_CLEANUP(connection_store_cleanup) =
		session_store_open_options(nullptr, &options);

	ASSERT_NE(store, nullptr);
	EXPECT_EQ(db_backend(&store->db), DB_BACKEND_SQLITE);
}

TEST(DatabaseConnectionTest, SQLiteSnapshotSurvivesSourceClose)
{
	ConnectionTestFile file;
	struct db source NB_CLEANUP(db_close) = {};
	struct db reopened NB_CLEANUP(db_close) = {};
	struct db_connection_config *connection NB_CLEANUP(db_connection_config_cleanup) = nullptr;
	struct session created = {};
	struct session loaded = {};
	struct db_options options = {};
	options.backend = DB_BACKEND_SQLITE;
	options.path = file.path;

	ASSERT_EQ(db_open_options(nullptr, &source, &options), NB_OK);
	EXPECT_EQ(db_backend(&source), DB_BACKEND_SQLITE);
	EXPECT_FALSE(db_is_memory(&source));
	ASSERT_EQ(db_init_schema(&source), NB_OK);
	ASSERT_EQ(session_create(&source, "connection-snapshot", "test", &created), NB_OK);
	ASSERT_EQ(db_connection_config_copy(&source, &connection), NB_OK);
	EXPECT_TRUE(db_connection_config_matches(connection, &source));
	db_close(&source);
	ASSERT_EQ(db_open_connection(nullptr, &reopened, connection), NB_OK);
	ASSERT_EQ(session_get_by_id(&reopened, created.id, &loaded), NB_OK);
	EXPECT_STREQ(loaded.name, "connection-snapshot");
	EXPECT_TRUE(db_connection_config_matches(connection, &reopened));
}

TEST(DatabaseConnectionTest, MemoryAndConfigurationValidation)
{
	struct db database NB_CLEANUP(db_close) = {};
	ASSERT_EQ(db_open(nullptr, &database, ":memory:"), NB_OK);
	EXPECT_TRUE(db_is_memory(&database));
	EXPECT_EQ(db_backend(&database), DB_BACKEND_SQLITE);
	EXPECT_EQ(db_reopen(nullptr, &database, &database), NB_ERR_INVAL);

	auto config = std::make_unique<struct config>();
	struct db_options options = {};
	config_set_defaults(nullptr, config.get());
	ASSERT_EQ(config_database_options(&config->database, "local.db", &options), NB_OK);
	EXPECT_EQ(options.backend, DB_BACKEND_SQLITE);
	EXPECT_EQ(options.query_timeout_ms, 30000);
	EXPECT_STREQ(options.url_env, "MORPH_DATABASE_URL");
	struct config_validation_error error = {};
	EXPECT_EQ(config_validate_text("[database]\ndriver='postgresql'\nurl_env='PG_URL'\n"
				       "tls_mode='disable'\nquery_timeout_ms=100\n",
				       &error),
		  NB_OK);
	EXPECT_NE(config_validate_text("[database]\nquery_timeout_ms=0\n", &error), NB_OK);
	EXPECT_NE(config_validate_text("[database]\ndriver='unknown'\n", &error), NB_OK);
}

TEST(DatabaseConnectionTest, LoadsPostgreSQLConnectionOptionsWithoutReadingCredentials)
{
	ConnectionTestFile file;
	{
		std::ofstream config(file.path);
		config << "[database]\ndriver='postgresql'\nurl_env='POSTGRES_PRIVATE_URL'\n"
			  "tls_mode='verify_ca'\nca_file='/certs/root.pem'\n"
			  "connect_timeout_ms=8000\nquery_timeout_ms=12000\n";
		ASSERT_TRUE(config.good());
	}

	auto config = std::make_unique<struct config>();
	ASSERT_EQ(config_load(nullptr, config.get(), file.path), NB_OK);
	struct db_options options = {};
	ASSERT_EQ(config_database_options(&config->database, "local.db", &options), NB_OK);
	EXPECT_EQ(options.backend, DB_BACKEND_POSTGRESQL);
	EXPECT_EQ(options.tls_mode, DB_TLS_VERIFY_CA);
	EXPECT_STREQ(options.url_env, "POSTGRES_PRIVATE_URL");
	EXPECT_STREQ(options.ca_file, "/certs/root.pem");
	EXPECT_EQ(options.connect_timeout_ms, 8000);
	EXPECT_EQ(options.query_timeout_ms, 12000);
}

static void connection_log(void *context, nb_log_level level, const char *record, size_t length)
{
	(void)level;
	static_cast<std::string *>(context)->append(record, length);
}

TEST(DatabaseConnectionTest, PostgreSQLDoesNotTreatUrlAsFilePath)
{
	struct db database NB_CLEANUP(db_close) = {};
	std::string logs;
	struct nb_log *logger = nb_log_new(NB_LOG_DEBUG, connection_log, &logs);
	ASSERT_NE(logger, nullptr);
	struct db_options options = {};
	options.backend = DB_BACKEND_POSTGRESQL;
	options.path = "postgresql://private-user:private-password@127.0.0.1/private-db";
	options.url_env = "MORPH_TEST_INTENTIONALLY_MISSING_URL";

	EXPECT_EQ(db_open_options(logger, &database, &options), NB_ERR_INVAL);
	EXPECT_FALSE(db_is_open(&database));
	EXPECT_TRUE(logs.find("private-password") == std::string::npos);
	EXPECT_STREQ(database.path, "");
	nb_log_free(logger);
}

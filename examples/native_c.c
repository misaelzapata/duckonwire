/* A real libduckdb C client. No Python, SQL adapter, or secret command arguments.
 * Usage: native-c-client ROOT [STATE_JSON]
 * The private descriptor is read by the local DuckDB JSON extension. */
#define _POSIX_C_SOURCE 200809L
#include "duckdb.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *failure_stage;
static duckdb_error_type failure_type = DUCKDB_ERROR_INVALID;
static const char *checks[16];
static size_t check_count;

static bool require(bool condition, const char *stage) {
    if (!condition && !failure_stage) failure_stage = stage;
    return condition;
}

static void passed(const char *name) { checks[check_count++] = name; }

static char *quote(const char *value) {
    size_t length = strlen(value), cursor = 0;
    char *result = malloc(length * 2 + 3);
    if (!result) return NULL;
    result[cursor++] = '\'';
    for (size_t i = 0; i < length; i++) {
        result[cursor++] = value[i];
        if (value[i] == '\'') result[cursor++] = '\'';
    }
    result[cursor++] = '\'';
    result[cursor] = 0;
    return result;
}

/* Diagnostics deliberately omit SQL and native error text: CONNECT embeds the
 * private token and an engine error may echo the input statement. */
static bool query(duckdb_connection con, const char *sql, duckdb_result *result, const char *stage) {
    memset(result, 0, sizeof(*result));
    bool ok = duckdb_query(con, sql, result) == DuckDBSuccess;
    if (!ok && !failure_stage) {
        failure_stage = stage;
        failure_type = duckdb_result_error_type(result);
    }
    return ok;
}

static bool command(duckdb_connection con, const char *sql, const char *stage) {
    duckdb_result result;
    bool ok = query(con, sql, &result, stage);
    duckdb_destroy_result(&result);
    return ok;
}

static void best_effort(duckdb_connection con, const char *sql) {
    duckdb_result result = {0};
    if (con) duckdb_query(con, sql, &result);
    duckdb_destroy_result(&result);
}

static bool scalar(duckdb_connection con, const char *sql, int64_t expected, const char *stage) {
    duckdb_result result;
    bool ok = query(con, sql, &result, stage);
    if (ok) ok = require(duckdb_row_count(&result) == 1 && duckdb_value_int64(&result, 0, 0) == expected, stage);
    duckdb_destroy_result(&result);
    return ok;
}

static bool text_value(duckdb_result *result, idx_t column, const char *expected, const char *stage) {
    char *value = duckdb_value_varchar(result, column, 0);
    bool ok = require(value && strcmp(value, expected) == 0, stage);
    duckdb_free(value);
    return ok;
}

static bool prepare(duckdb_connection con, const char *sql, duckdb_prepared_statement *statement, const char *stage) {
    return require(duckdb_prepare(con, sql, statement) == DuckDBSuccess, stage);
}

static bool execute(duckdb_prepared_statement statement, duckdb_result *result, const char *stage) {
    memset(result, 0, sizeof(*result));
    bool ok = duckdb_execute_prepared(statement, result) == DuckDBSuccess;
    if (!ok && !failure_stage) {
        failure_stage = stage;
        failure_type = duckdb_result_error_type(result);
    }
    return ok;
}

static bool execute_command(duckdb_prepared_statement statement, const char *stage) {
    duckdb_result result;
    bool ok = execute(statement, &result, stage);
    duckdb_destroy_result(&result);
    return ok;
}

static bool bound_label(duckdb_connection con, const char *table, int64_t id, const char *label) {
    char sql[256];
    duckdb_prepared_statement statement = NULL;
    duckdb_result result = {0};
    snprintf(sql, sizeof(sql), "SELECT label FROM %s WHERE id=?", table);
    bool ok = prepare(con, sql, &statement, "prepare_bound_label") &&
              require(duckdb_bind_int64(statement, 1, id) == DuckDBSuccess, "bind_label_id") &&
              execute(statement, &result, "execute_bound_label") &&
              require(duckdb_row_count(&result) == 1, "bound_label_single_row") &&
              text_value(&result, 0, label, "bound_label_value");
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&statement);
    return ok;
}

static bool named(duckdb_prepared_statement statement, const char *name, idx_t *index) {
    return require(duckdb_bind_parameter_index(statement, index, name) == DuckDBSuccess, "native_named_parameter_index");
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *value = (const unsigned char *)(text ? text : ""); *value; value++) {
        if (*value == '"' || *value == '\\') printf("\\%c", *value);
        else if (*value < 32) printf("\\u%04x", *value);
        else putchar(*value);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    duckdb_database db = NULL;
    duckdb_connection a = NULL, b = NULL;
    duckdb_config config = NULL;
    duckdb_prepared_statement insert = NULL, select = NULL, update = NULL, named_insert = NULL;
    duckdb_result result = {0};
    duckdb_logical_type integer = NULL, text = NULL, structure = NULL;
    duckdb_value children[3] = {NULL, NULL, NULL}, members[2] = {NULL, NULL};
    duckdb_value list_value = NULL, struct_value = NULL;
    char *endpoint = NULL, *token = NULL;
    char *quoted = NULL, *quoted_endpoint = NULL, *quoted_token = NULL;
    char descriptor[4096], sql[16384], table[80];
    bool created = false, cleanup_ok = false, ok = false, cross_language = false;
    snprintf(table, sizeof(table), "codex_native_c_%ld", (long)getpid());
    if (!require(argc >= 2 && argc <= 3, "usage_ROOT_optional_STATE_JSON")) goto cleanup;
    if (argc == 3) snprintf(descriptor, sizeof(descriptor), "%s", argv[2]);
    else snprintf(descriptor, sizeof(descriptor), "%s/.runtime/native.json", argv[1]);
    if (!require(duckdb_create_config(&config) == DuckDBSuccess &&
                 duckdb_set_config(config, "threads", "2") == DuckDBSuccess, "native_configuration")) goto cleanup;
    if (!require(duckdb_open_ext(NULL, &db, config, NULL) == DuckDBSuccess &&
                 duckdb_connect(db, &a) == DuckDBSuccess && duckdb_connect(db, &b) == DuckDBSuccess, "native_database_and_connections")) goto cleanup;
    quoted = quote(descriptor);
    if (!require(quoted != NULL, "descriptor_allocation")) goto cleanup;
    snprintf(sql, sizeof(sql), "SELECT endpoint,token FROM read_json_auto(%s)", quoted);
    free(quoted); quoted = NULL;
    if (!query(a, sql, &result, "native_json_descriptor") || !require(duckdb_row_count(&result) == 1, "private_descriptor_one_row")) goto cleanup;
    endpoint = duckdb_value_varchar(&result, 0, 0);
    token = duckdb_value_varchar(&result, 1, 0);
    duckdb_destroy_result(&result);
    if (!require(endpoint && token, "private_descriptor_fields")) goto cleanup;
    if (!query(a, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') AND loaded AND installed AND install_mode='STATICALLY_LINKED'", &result, "builtin_extensions")) goto cleanup;
    if (!require(duckdb_value_int64(&result, 0, 0) == 2, "native_extensions_loaded_by_default")) goto cleanup;
    duckdb_destroy_result(&result);
    passed("httpfs_and_quack_builtin_without_LOAD");
    quoted_endpoint = quote(endpoint); quoted_token = quote(token);
    if (!require(quoted_endpoint && quoted_token, "connect_allocation")) goto cleanup;
    snprintf(sql, sizeof(sql), "CONNECT %s (TOKEN %s)", quoted_endpoint, quoted_token);
    if (!command(a, sql, "connect_first_native_session") || !command(b, sql, "connect_second_native_session")) goto cleanup;
    memset(sql, 0, sizeof(sql));
    free(quoted_endpoint); quoted_endpoint = NULL;
    memset(quoted_token, 0, strlen(quoted_token)); free(quoted_token); quoted_token = NULL;
    passed("two_native_C_connections_same_Quack_endpoint");

    snprintf(sql, sizeof(sql), "CREATE TABLE %s(id BIGINT PRIMARY KEY,label VARCHAR NOT NULL,amount DECIMAL(20,4),day DATE,bytes BLOB,huge HUGEINT,seq INTEGER[],item STRUCT(name VARCHAR,count INTEGER),optional INTEGER)", table);
    if (!command(a, sql, "create_C_probe_table")) goto cleanup;
    created = true;
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (?,?,?,?,?,?,?,?,?)", table);
    if (!prepare(a, sql, &insert, "prepare_positional_insert") || !require(duckdb_nparams(insert) == 9, "nine_native_parameter_slots")) goto cleanup;
    duckdb_decimal amount = {20, 4, {12345678901234ULL, 0}};
    duckdb_hugeint huge = {0, (int64_t)1 << 36};
    duckdb_date day = duckdb_to_date((duckdb_date_struct){2026, 10, 8});
    const unsigned char bytes[] = {0, 255, 1};
    integer = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    text = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_logical_type member_types[] = {text, integer};
    const char *member_names[] = {"name", "count"};
    structure = duckdb_create_struct_type(member_types, member_names, 2);
    children[0] = duckdb_create_int32(1); children[1] = duckdb_create_null_value(); children[2] = duckdb_create_int32(3);
    members[0] = duckdb_create_varchar("mañana"); members[1] = duckdb_create_int32(7);
    list_value = duckdb_create_list_value(integer, children, 3);
    struct_value = duckdb_create_struct_value(structure, members);
    if (!require(list_value && struct_value &&
                 duckdb_bind_int64(insert, 1, 1) == DuckDBSuccess &&
                 duckdb_bind_varchar(insert, 2, "C nativo: ñ ' ? $") == DuckDBSuccess &&
                 duckdb_bind_decimal(insert, 3, amount) == DuckDBSuccess &&
                 duckdb_bind_date(insert, 4, day) == DuckDBSuccess &&
                 duckdb_bind_blob(insert, 5, bytes, sizeof(bytes)) == DuckDBSuccess &&
                 duckdb_bind_hugeint(insert, 6, huge) == DuckDBSuccess &&
                 duckdb_bind_value(insert, 7, list_value) == DuckDBSuccess &&
                 duckdb_bind_value(insert, 8, struct_value) == DuckDBSuccess &&
                 duckdb_bind_null(insert, 9) == DuckDBSuccess, "native_typed_C_bindings")) goto cleanup;
    if (!execute_command(insert, "execute_typed_prepared_insert")) goto cleanup;
    snprintf(sql, sizeof(sql), "SELECT id,label,amount,day,bytes,huge,seq,item,optional,seq[1],seq[2],seq[3],item.name,item.count FROM %s WHERE id=?", table);
    if (!prepare(b, sql, &select, "prepare_typed_select") || !require(duckdb_bind_int64(select, 1, 1) == DuckDBSuccess, "bind_select_id") || !execute(select, &result, "execute_typed_select")) goto cleanup;
    if (!require(duckdb_row_count(&result) == 1 && duckdb_column_count(&result) == 14 &&
                 strcmp(duckdb_column_name(&result, 1), "label") == 0 &&
                 duckdb_column_type(&result, 2) == DUCKDB_TYPE_DECIMAL &&
                 duckdb_column_type(&result, 3) == DUCKDB_TYPE_DATE &&
                 duckdb_column_type(&result, 4) == DUCKDB_TYPE_BLOB &&
                 duckdb_column_type(&result, 5) == DUCKDB_TYPE_HUGEINT &&
                 duckdb_column_type(&result, 6) == DUCKDB_TYPE_LIST &&
                 duckdb_column_type(&result, 7) == DUCKDB_TYPE_STRUCT, "native_result_schema_types")) goto cleanup;
    duckdb_decimal actual_amount = duckdb_value_decimal(&result, 2, 0);
    duckdb_hugeint actual_huge = duckdb_value_hugeint(&result, 5, 0);
    duckdb_blob actual_blob = duckdb_value_blob(&result, 4, 0);
    bool blob_equal = actual_blob.size == sizeof(bytes) && memcmp(actual_blob.data, bytes, sizeof(bytes)) == 0;
    duckdb_free(actual_blob.data);
    if (!require(duckdb_value_int64(&result, 0, 0) == 1 &&
                 actual_amount.width == 20 && actual_amount.scale == 4 && actual_amount.value.lower == amount.value.lower && actual_amount.value.upper == 0 &&
                 duckdb_value_date(&result, 3, 0).days == day.days && blob_equal &&
                 actual_huge.lower == huge.lower && actual_huge.upper == huge.upper &&
                 duckdb_value_is_null(&result, 8, 0) && duckdb_value_is_null(&result, 10, 0) &&
                 duckdb_value_int64(&result, 9, 0) == 1 && duckdb_value_int64(&result, 11, 0) == 3 && duckdb_value_int64(&result, 13, 0) == 7, "lossless_native_C_values") ||
        !text_value(&result, 1, "C nativo: ñ ' ? $", "unicode_literal_parameter") || !text_value(&result, 12, "mañana", "nested_struct_unicode")) goto cleanup;
    duckdb_destroy_result(&result); duckdb_destroy_prepare(&select);
    passed("prepared_positional_lossless_decimal_date_blob_hugeint_list_struct_NULL");
    if (!require(duckdb_execute_prepared(insert, &result) == DuckDBError && duckdb_result_error_type(&result) == DUCKDB_ERROR_CONSTRAINT, "native_constraint_error_type")) goto cleanup;
    duckdb_destroy_result(&result); duckdb_destroy_prepare(&insert);
    if (!scalar(a, "SELECT 9", 9, "C_connection_reuse_after_constraint_error")) goto cleanup;
    passed("native_constraint_exception_and_connection_reuse");

    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES ($id,$label,$amount,DATE '2026-10-08',NULL,NULL,NULL,NULL,$optional)", table);
    if (!prepare(a, sql, &named_insert, "prepare_named_insert")) goto cleanup;
    idx_t id_index, label_index, amount_index, optional_index;
    if (!named(named_insert, "id", &id_index) || !named(named_insert, "label", &label_index) || !named(named_insert, "amount", &amount_index) || !named(named_insert, "optional", &optional_index)) goto cleanup;
    amount.value.lower = 25000;
    if (!require(duckdb_bind_int64(named_insert, id_index, 2) == DuckDBSuccess && duckdb_bind_varchar(named_insert, label_index, "named") == DuckDBSuccess && duckdb_bind_decimal(named_insert, amount_index, amount) == DuckDBSuccess && duckdb_bind_null(named_insert, optional_index) == DuckDBSuccess, "native_named_bindings") || !execute_command(named_insert, "execute_named_insert")) goto cleanup;
    amount.value.lower = 31250;
    if (!require(duckdb_clear_bindings(named_insert) == DuckDBSuccess && duckdb_bind_int64(named_insert, id_index, 3) == DuckDBSuccess && duckdb_bind_varchar(named_insert, label_index, "reused") == DuckDBSuccess && duckdb_bind_decimal(named_insert, amount_index, amount) == DuckDBSuccess && duckdb_bind_int32(named_insert, optional_index, 7) == DuckDBSuccess, "native_reused_prepared_bindings") || !execute_command(named_insert, "execute_reused_insert")) goto cleanup;
    duckdb_destroy_prepare(&named_insert);
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s WHERE (id=2 AND optional IS NULL) OR (id=3 AND optional=7)", table);
    if (!scalar(b, sql, 2, "named_NULL_then_non_NULL_values")) goto cleanup;
    passed("native_named_parameter_indexes_and_prepared_reuse_NULL_to_integer");

    if (!command(b, "BEGIN", "reader_begin") || !bound_label(b, table, 1, "C nativo: ñ ' ? $") || !command(a, "BEGIN", "writer_begin")) goto cleanup;
    snprintf(sql, sizeof(sql), "UPDATE %s SET label=? WHERE id=?", table);
    if (!prepare(a, sql, &update, "prepare_transaction_update") || !require(duckdb_bind_varchar(update, 1, "committed from C") == DuckDBSuccess && duckdb_bind_int64(update, 2, 1) == DuckDBSuccess, "bind_transaction_update") || !execute_command(update, "execute_transaction_update")) goto cleanup;
    duckdb_destroy_prepare(&update);
    if (!bound_label(b, table, 1, "C nativo: ñ ' ? $") || !command(a, "COMMIT", "writer_commit") || !bound_label(b, table, 1, "C nativo: ñ ' ? $") || !command(b, "COMMIT", "reader_commit") || !bound_label(b, table, 1, "committed from C")) goto cleanup;
    passed("independent_C_sessions_MVCC_snapshot_and_COMMIT_visibility");
    if (!command(a, "BEGIN", "rollback_begin")) goto cleanup;
    snprintf(sql, sizeof(sql), "UPDATE %s SET label='must rollback' WHERE id=1", table);
    if (!command(a, sql, "rollback_update") || !command(a, "ROLLBACK", "explicit_rollback") || !bound_label(b, table, 1, "committed from C")) goto cleanup;
    passed("native_SQL_ROLLBACK_discards_pending_write");
    if (!command(a, "BEGIN", "close_rollback_begin") || !command(a, sql, "close_rollback_update")) goto cleanup;
    duckdb_disconnect(&a);
    if (!bound_label(b, table, 1, "committed from C")) goto cleanup;
    passed("native_C_disconnect_rolls_back_pending_transaction");

    if (!query(b, "SELECT count(*) FROM information_schema.tables WHERE table_name='cross_language_probe'", &result, "cross_language_table_discovery")) goto cleanup;
    cross_language = duckdb_value_int64(&result, 0, 0) == 1;
    duckdb_destroy_result(&result);
    if (cross_language) {
        if (!bound_label(b, "cross_language_probe", 0, "from Python") || !bound_label(b, "cross_language_probe", 1, "from C++")) goto cleanup;
        if (!prepare(b, "INSERT INTO cross_language_probe VALUES (?,?)", &insert, "prepare_cross_language_insert") || !require(duckdb_bind_int32(insert, 1, 2) == DuckDBSuccess && duckdb_bind_varchar(insert, 2, "from C") == DuckDBSuccess, "cross_language_C_bindings") || !execute_command(insert, "execute_cross_language_C_insert")) goto cleanup;
        duckdb_destroy_prepare(&insert);
        if (!bound_label(b, "cross_language_probe", 2, "from C")) goto cleanup;
        passed("C_reads_bound_Python_and_CPP_rows_and_inserts_bound_C_row");
    }
    if (!query(b, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'", &result, "multilingual_table_discovery")) goto cleanup;
    bool multilingual = duckdb_value_int64(&result, 0, 0) == 1;
    duckdb_destroy_result(&result);
    if (multilingual) {
        if (!prepare(b, "INSERT INTO lang_multiclient_probe VALUES (?,?)", &insert, "prepare_multilingual_C_insert") || !require(duckdb_bind_int32(insert, 1, 2) == DuckDBSuccess && duckdb_bind_varchar(insert, 2, "from C") == DuckDBSuccess, "multilingual_C_bindings") || !execute_command(insert, "execute_multilingual_C_insert")) goto cleanup;
        duckdb_destroy_prepare(&insert);
        if (!bound_label(b, "lang_multiclient_probe", 2, "from C")) goto cleanup;
        passed("C_native_marker_visible_to_other_languages");
    }
    ok = true;

cleanup:
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&insert); duckdb_destroy_prepare(&select); duckdb_destroy_prepare(&update); duckdb_destroy_prepare(&named_insert);
    for (size_t i = 0; i < 3; i++) duckdb_destroy_value(&children[i]);
    for (size_t i = 0; i < 2; i++) duckdb_destroy_value(&members[i]);
    duckdb_destroy_value(&list_value); duckdb_destroy_value(&struct_value);
    duckdb_destroy_logical_type(&integer); duckdb_destroy_logical_type(&text); duckdb_destroy_logical_type(&structure);
    if (created) {
        best_effort(a, "ROLLBACK"); best_effort(b, "ROLLBACK");
        snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", table);
        cleanup_ok = command(b ? b : a, sql, "C_probe_cleanup");
        if (cleanup_ok) {
            snprintf(sql, sizeof(sql), "SELECT count(*) FROM information_schema.tables WHERE table_name='%s'", table);
            cleanup_ok = scalar(b ? b : a, sql, 0, "C_probe_table_absent_after_cleanup");
        }
    }
    if (created && !cleanup_ok) ok = false;
    if (cleanup_ok) passed("temporary_C_probe_table_removed");
    duckdb_disconnect(&a); duckdb_disconnect(&b); duckdb_close(&db); duckdb_destroy_config(&config);
    free(quoted); free(quoted_endpoint);
    if (quoted_token) { memset(quoted_token, 0, strlen(quoted_token)); free(quoted_token); }
    if (token) { memset(token, 0, strlen(token)); duckdb_free(token); }
    printf("{\n  \"scope\":\"direct libduckdb C API; no Python bridge or connection wrapper\",\n  \"language\":\"C11\",\n  \"library_version\":");
    json_string(duckdb_library_version());
    printf(",\n  \"endpoint\":"); json_string(endpoint); duckdb_free(endpoint);
    printf(",\n  \"python_bridge\":false,\n  \"cross_language_table_present\":%s,\n  \"passed\":%s,\n  \"checks\":[", cross_language ? "true" : "false", ok ? "true" : "false");
    for (size_t i = 0; i < check_count; i++) {
        if (i) putchar(',');
        printf("\n    {\"name\":"); json_string(checks[i]); printf(",\"passed\":true}");
    }
    printf("\n  ],\n  \"failure_stage\":");
    if (failure_stage) json_string(failure_stage); else printf("null");
    printf(",\n  \"native_error_type\":%d\n}\n", (int)failure_type);
    return ok ? 0 : 1;
}

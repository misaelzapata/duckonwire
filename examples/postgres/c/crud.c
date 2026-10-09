#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CLIENT_LABEL
#define CLIENT_LABEL "PostgreSQL C"
#endif

static void checked(PGconn *connection, PGresult *result) {
    if (PQresultStatus(result) != PGRES_COMMAND_OK && PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s", PQresultErrorMessage(result));
        PQclear(result); PQfinish(connection); exit(1);
    }
}

static void command(PGconn *connection, const char *sql) {
    PGresult *result = PQexec(connection, sql); checked(connection, result); PQclear(result);
}

static void bound(PGconn *connection, const char *sql, int count, const char *const *values) {
    PGresult *result = PQexecParams(connection, sql, count, NULL, values, NULL, NULL, 0);
    checked(connection, result); PQclear(result);
}

int main(void) {
    const char *host = getenv("PGHOST"), *port = getenv("PGPORT"), *user = getenv("PGUSER");
    const char *database = getenv("PGDATABASE"), *password = getenv("PGPASSWORD");
    if (!password) password = getenv("DUCKONWIRE_TOKEN");
    const char *sslmode = getenv("PGSSLMODE");
    if (!sslmode) sslmode = "disable";
    if (strcmp(sslmode,"disable") && strcmp(sslmode,"verify-full")) { fprintf(stderr,"Use PGSSLMODE=disable or verify-full\n"); return 2; }
    if (!strcmp(sslmode,"verify-full") && !getenv("PGSSLROOTCERT")) { fprintf(stderr,"Set PGSSLROOTCERT for verify-full\n"); return 2; }
    const char *keys[] = {"host", "port", "user", "dbname", "password", "sslmode", "connect_timeout", NULL};
    const char *values[] = {host ? host : "127.0.0.1", port ? port : "9496", user ? user : "duckonwire",
                           database ? database : "duckonwire", password ? password : "", sslmode, "5", NULL};
    PGconn *connection = PQconnectdbParams(keys, values, 0);
    if (PQstatus(connection) != CONNECTION_OK) { fprintf(stderr, "%s", PQerrorMessage(connection)); PQfinish(connection); return 1; }
    command(connection, "CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))");
    command(connection, "BEGIN");
    const char *owner[] = {CLIENT_LABEL};
    bound(connection, "DELETE FROM pg_demo_products WHERE client=$1", 1, owner);
    PGresult *prepared = PQprepare(connection, "insert_product", "INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)", 5, NULL);
    checked(connection, prepared); PQclear(prepared);
    const char *products[2][5] = {{CLIENT_LABEL, "1", "Cafe", "19.99", "3"}, {CLIENT_LABEL, "2", "Arroz", "8.50", "10"}};
    for (int i = 0; i < 2; i++) {
        PGresult *inserted = PQexecPrepared(connection, "insert_product", 5, products[i], NULL, NULL, 0);
        checked(connection, inserted); PQclear(inserted);
    }
    const char *update[] = {CLIENT_LABEL, "1"}, *remove[] = {CLIENT_LABEL, "2"};
    bound(connection, "UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2", 2, update);
    bound(connection, "DELETE FROM pg_demo_products WHERE client=$1 AND id=$2", 2, remove);
    command(connection, "COMMIT");
    PGresult *selected = PQexecParams(connection, "SELECT id,name,price,stock,json_array(id,name,CAST(price AS VARCHAR),stock) FROM pg_demo_products WHERE client=$1 ORDER BY id", 1, NULL, owner, NULL, NULL, 0);
    checked(connection, selected);
    if (PQntuples(selected) != 1 || strcmp(PQgetvalue(selected,0,0), "1") || strcmp(PQgetvalue(selected,0,1), "Cafe") || strcmp(PQgetvalue(selected,0,2), "19.99") || strcmp(PQgetvalue(selected,0,3), "4")) {
        fprintf(stderr, "Unexpected committed row\n"); PQclear(selected); PQfinish(connection); return 1;
    }
    printf("{\"passed\":true,\"client\":\"%s\",\"driver\":\"libpq\",\"rows\":[%s]}\n", CLIENT_LABEL, PQgetvalue(selected,0,4));
    PQclear(selected); PQfinish(connection); return 0;
}

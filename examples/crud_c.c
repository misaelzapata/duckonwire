#include "duckdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static duckdb_database db;
static duckdb_connection con;
static void require(int success) {
    if (!success) {
        fputs("Native CRUD failed\n",stderr);
        if (con) duckdb_disconnect(&con);
        if (db) duckdb_close(&db);
        exit(1);
    }
}
static void query(duckdb_connection con,const char *sql) {
    duckdb_result result; require(duckdb_query(con,sql,&result)==DuckDBSuccess); duckdb_destroy_result(&result);
}
static char *quote(const char *value) {
    char *out=malloc(strlen(value)*2+3), *p=out; *p++='\'';
    for (;*value;value++) { *p++=*value; if (*value=='\'') *p++='\''; }
    *p++='\''; *p=0; return out;
}
static void insert(duckdb_connection con,int id,const char *name,const char *price,int stock) {
    duckdb_prepared_statement stmt;duckdb_result result;
    require(duckdb_prepare(con,"INSERT INTO demo_products VALUES (?,?,?,?,?)",&stmt)==DuckDBSuccess);
    require(duckdb_bind_varchar(stmt,1,"C")==DuckDBSuccess);
    require(duckdb_bind_int32(stmt,2,id)==DuckDBSuccess);
    require(duckdb_bind_varchar(stmt,3,name)==DuckDBSuccess);
    require(duckdb_bind_varchar(stmt,4,price)==DuckDBSuccess);
    require(duckdb_bind_int32(stmt,5,stock)==DuckDBSuccess);
    require(duckdb_execute_prepared(stmt,&result)==DuckDBSuccess);
    duckdb_destroy_result(&result);duckdb_destroy_prepare(&stmt);
}
int main(void) {
    duckdb_prepared_statement stmt;duckdb_result result;
    const char *endpoint=getenv("DUCKONWIRE_ENDPOINT"), *token=getenv("DUCKONWIRE_TOKEN");
    if(!endpoint)endpoint="quack:127.0.0.1:9495";
    if(!token) { fputs("Set DUCKONWIRE_TOKEN\n",stderr);return 2; }
    require(duckdb_open(NULL,&db)==DuckDBSuccess);require(duckdb_connect(db,&con)==DuckDBSuccess);
    char *uri=quote(endpoint),*secret=quote(token),*sql=malloc(strlen(uri)+strlen(secret)+24);
    sprintf(sql,"CONNECT %s (TOKEN %s)",uri,secret);query(con,sql);
    free(sql);free(uri);free(secret);
    query(con,"CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))");
    query(con,"BEGIN");query(con,"DELETE FROM demo_products WHERE client='C'");
    insert(con,1,"Cafe","19.99",3);insert(con,2,"Arroz","8.50",10);
    require(duckdb_prepare(con,"UPDATE demo_products SET stock=stock+? WHERE client=? AND id=?",&stmt)==DuckDBSuccess);
    require(duckdb_bind_int32(stmt,1,1)==DuckDBSuccess && duckdb_bind_varchar(stmt,2,"C")==DuckDBSuccess && duckdb_bind_int32(stmt,3,1)==DuckDBSuccess);
    require(duckdb_execute_prepared(stmt,&result)==DuckDBSuccess);duckdb_destroy_result(&result);duckdb_destroy_prepare(&stmt);
    require(duckdb_query(con,"SELECT id,name,price,stock FROM demo_products WHERE client='C' ORDER BY id",&result)==DuckDBSuccess);
    require(duckdb_row_count(&result)==2 && duckdb_value_int32(&result,3,0)==4);duckdb_destroy_result(&result);
    query(con,"DELETE FROM demo_products WHERE client='C' AND id=2");query(con,"COMMIT");
    require(duckdb_query(con,"SELECT id,name,price,stock FROM demo_products WHERE client='C' ORDER BY id",&result)==DuckDBSuccess);
    char *price=duckdb_value_varchar(&result,2,0);
    require(duckdb_row_count(&result)==1 && strcmp(price,"19.99")==0 && duckdb_value_int32(&result,3,0)==4);
    duckdb_free(price);duckdb_destroy_result(&result);
    require(duckdb_query(con,"SELECT json_array(id,name,CAST(price AS VARCHAR),stock) FROM demo_products WHERE client='C' ORDER BY id",&result)==DuckDBSuccess);
    char *json=duckdb_value_varchar(&result,0,0);
    printf("{\"passed\":true,\"language\":\"C\",\"rows\":[%s]}\n",json);
    duckdb_free(json);duckdb_destroy_result(&result);duckdb_disconnect(&con);duckdb_close(&db);return 0;
}

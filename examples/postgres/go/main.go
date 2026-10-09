package main

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/jackc/pgx/v5"
	"os"
)

func main() {
	ctx := context.Background()
	for key, value := range map[string]string{"PGHOST": "127.0.0.1", "PGPORT": "9496", "PGUSER": "duckonwire", "PGDATABASE": "duckonwire", "PGSSLMODE": "disable"} {
		if os.Getenv(key) == "" {
			os.Setenv(key, value)
		}
	}
	mode := os.Getenv("PGSSLMODE")
	if mode != "disable" && mode != "verify-full" {
		panic("Use PGSSLMODE=disable or verify-full")
	}
	if mode == "verify-full" && os.Getenv("PGSSLROOTCERT") == "" {
		panic("Set PGSSLROOTCERT for verify-full")
	}
	config, err := pgx.ParseConfig("")
	if err != nil {
		panic(err)
	}
	if config.Password == "" {
		config.Password = os.Getenv("DUCKONWIRE_TOKEN")
	}
	config.Fallbacks = nil
	connection, err := pgx.ConnectConfig(ctx, config)
	if err != nil {
		panic(err)
	}
	defer connection.Close(ctx)
	owner := "PostgreSQL Go"
	if _, err = connection.Exec(ctx, "CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))"); err != nil {
		panic(err)
	}
	transaction, err := connection.Begin(ctx)
	if err != nil {
		panic(err)
	}
	defer transaction.Rollback(ctx)
	if _, err = transaction.Exec(ctx, "DELETE FROM pg_demo_products WHERE client=$1", owner); err != nil {
		panic(err)
	}
	for _, row := range [][]any{{1, "Cafe", "19.99", 3}, {2, "Arroz", "8.50", 10}} {
		if _, err = transaction.Exec(ctx, "INSERT INTO pg_demo_products VALUES($1,$2,$3,CAST($4 AS VARCHAR)::DECIMAL(18,2),$5)", append([]any{owner}, row...)...); err != nil {
			panic(err)
		}
	}
	if _, err = transaction.Exec(ctx, "UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2", owner, 1); err != nil {
		panic(err)
	}
	if _, err = transaction.Exec(ctx, "DELETE FROM pg_demo_products WHERE client=$1 AND id=$2", owner, 2); err != nil {
		panic(err)
	}
	if err = transaction.Commit(ctx); err != nil {
		panic(err)
	}
	rows, err := connection.Query(ctx, "SELECT id,name,CAST(price AS VARCHAR),stock FROM pg_demo_products WHERE client=$1 ORDER BY id", owner)
	if err != nil {
		panic(err)
	}
	defer rows.Close()
	selected := [][]any{}
	for rows.Next() {
		var id, stock int
		var name, price string
		if err = rows.Scan(&id, &name, &price, &stock); err != nil {
			panic(err)
		}
		selected = append(selected, []any{id, name, price, stock})
	}
	if err = rows.Err(); err != nil {
		panic(err)
	}
	if len(selected) != 1 || selected[0][0] != 1 || selected[0][1] != "Cafe" || selected[0][2] != "19.99" || selected[0][3] != 4 {
		panic("Unexpected committed row")
	}
	output, _ := json.Marshal(map[string]any{"passed": true, "client": owner, "driver": "pgx/v5", "rows": selected})
	fmt.Println(string(output))
}

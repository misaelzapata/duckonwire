// Ordinary CRUD with the official database/sql driver and native CONNECT.
package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"

	duckdb "github.com/duckdb/duckdb-go/v2"
)

func literal(value string) string { return "'" + strings.ReplaceAll(value, "'", "''") + "'" }

func crud(endpoint, token string) ([][]any, error) {
	connector, err := duckdb.NewConnector(":memory:", func(conn driver.ExecerContext) error {
		_, err := conn.ExecContext(context.Background(), "CONNECT "+literal(endpoint)+" (TOKEN "+literal(token)+")", nil)
		return err
	})
	if err != nil {
		return nil, err
	}
	db := sql.OpenDB(connector)
	defer db.Close()
	db.SetMaxOpenConns(1)
	if _, err := db.Exec("CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))"); err != nil {
		return nil, err
	}

	tx, err := db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	// Repeating this example replaces only this client's products.
	if _, err := tx.Exec("DELETE FROM demo_products WHERE client=?", "Go"); err != nil {
		return nil, err
	}
	for _, product := range [][]any{{1, "Cafe", "19.99", 3}, {2, "Arroz", "8.50", 10}} {
		if _, err := tx.Exec("INSERT INTO demo_products VALUES(?,?,?,?,?)", "Go", product[0], product[1], product[2], product[3]); err != nil {
			return nil, err
		}
	}
	if _, err := tx.Exec("UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?", "Go", 1); err != nil {
		return nil, err
	}
	rows, err := tx.Query("SELECT id,name,CAST(price AS VARCHAR),stock FROM demo_products WHERE client=? ORDER BY id", "Go")
	if err != nil {
		return nil, err
	}
	seen := 0
	for rows.Next() {
		var id, stock int32
		var name, price string
		if err := rows.Scan(&id, &name, &price, &stock); err != nil {
			rows.Close()
			return nil, err
		}
		seen++
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	if seen != 2 {
		return nil, errors.New("expected both products before DELETE")
	}
	if _, err := tx.Exec("DELETE FROM demo_products WHERE client=? AND id=?", "Go", 2); err != nil {
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}

	var id, stock int32
	var name, price string
	if err := db.QueryRow("SELECT id,name,CAST(price AS VARCHAR),stock FROM demo_products WHERE client=? AND id=?", "Go", 1).Scan(&id, &name, &price, &stock); err != nil {
		return nil, err
	}
	if id != 1 || name != "Cafe" || price != "19.99" || stock != 4 {
		return nil, errors.New("committed product does not match CRUD operations")
	}
	return [][]any{{id, name, price, stock}}, nil
}

func main() {
	endpoint := os.Getenv("DUCKONWIRE_ENDPOINT")
	if endpoint == "" {
		endpoint = "quack:127.0.0.1:9495"
	}
	token := os.Getenv("DUCKONWIRE_TOKEN")
	var rows [][]any
	err := func() error {
		if token == "" {
			return errors.New("DUCKONWIRE_TOKEN is required")
		}
		var err error
		rows, err = crud(endpoint, token)
		return err
	}()
	if err != nil {
		message := err.Error()
		if token != "" {
			message = strings.ReplaceAll(message, token, "[REDACTED]")
		}
		json.NewEncoder(os.Stdout).Encode(map[string]any{"passed": false, "language": "Go", "error": message})
		os.Exit(1)
	}
	if err := json.NewEncoder(os.Stdout).Encode(map[string]any{"passed": true, "language": "Go", "rows": rows}); err != nil {
		fmt.Fprintln(os.Stderr, "could not serialize result")
		os.Exit(1)
	}
}

// The unmodified official database/sql driver links to the native C library.
package main

import (
	"bytes"
	"context"
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"errors"
	"fmt"
	duckdb "github.com/duckdb/duckdb-go/v2"
	"github.com/duckdb/duckdb-go/v2/mapping"
	"io"
	"math/big"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"time"
)

type state struct {
	URI      string `json:"uri"`
	Endpoint string `json:"endpoint"`
	Token    string `json:"token"`
}
type builtinExtension struct {
	Name        string `json:"extension_name"`
	Loaded      bool   `json:"loaded"`
	Installed   bool   `json:"installed"`
	InstallMode string `json:"install_mode"`
	InstallPath string `json:"install_path"`
}
type check struct {
	Name         string  `json:"name"`
	Passed       bool    `json:"passed"`
	Milliseconds float64 `json:"milliseconds"`
}
type output struct {
	Passed               bool               `json:"passed"`
	NativeUsed           bool               `json:"native_used"`
	Driver               string             `json:"driver"`
	NativeVersion        string             `json:"native_version"`
	ClientLibraryVersion string             `json:"client_library_version"`
	Checks               []check            `json:"checks"`
	Limits               []string           `json:"limits"`
	Error                string             `json:"error,omitempty"`
	Stage                string             `json:"stage,omitempty"`
	Interop              bool               `json:"multiclient_shared_state,omitempty"`
	RowsSeen             []sharedRow        `json:"multiclient_rows_seen,omitempty"`
	BuiltinExtensions    []builtinExtension `json:"builtin_extensions_before_connect"`
}
type sharedRow struct {
	ID    int32  `json:"id"`
	Label string `json:"label"`
}

func literal(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }
func require(ok bool, message string) error {
	if !ok {
		return errors.New(message)
	}
	return nil
}
func verifyBuiltins(conn driver.ExecerContext, proof *[]builtinExtension) error {
	queryer, ok := conn.(driver.QueryerContext)
	if !ok {
		return errors.New("official driver does not expose native query initialization")
	}
	rows, err := queryer.QueryContext(context.Background(), "SELECT extension_name,loaded,installed,install_mode,install_path FROM duckdb_extensions() WHERE extension_name IN ('httpfs','quack') ORDER BY extension_name", nil)
	if err != nil {
		return err
	}
	defer rows.Close()
	var extensions []builtinExtension
	for {
		values := make([]driver.Value, 5)
		if err := rows.Next(values); err != nil {
			if err == io.EOF {
				break
			}
			return err
		}
		name, nameOK := values[0].(string)
		loaded, loadedOK := values[1].(bool)
		installed, installedOK := values[2].(bool)
		mode, modeOK := values[3].(string)
		path, pathOK := values[4].(string)
		if !(nameOK && loadedOK && installedOK && modeOK && pathOK) {
			return errors.New("native extension metadata has unexpected types")
		}
		extensions = append(extensions, builtinExtension{name, loaded, installed, mode, path})
	}
	if proof != nil {
		*proof = extensions
	}
	if len(extensions) != 2 {
		return errors.New("httpfs and quack must be built in before CONNECT")
	}
	for i, expected := range []string{"httpfs", "quack"} {
		extension := extensions[i]
		if extension.Name != expected || !extension.Loaded || !extension.Installed || extension.InstallMode != "STATICALLY_LINKED" || extension.InstallPath != "(BUILT-IN)" {
			return fmt.Errorf("extension %s is not loaded as a native built-in before CONNECT", expected)
		}
	}
	return nil
}
func open(root string, descriptor state, proof *[]builtinExtension) (*sql.DB, error) {
	temporary := filepath.Join(root, ".native/languages/go/tmp")
	if _, err := os.Stat(filepath.Join(root, "bin/duckonwire-server")); err == nil {
		temporary = filepath.Join(root, "data/tmp-go")
	}
	if err := os.MkdirAll(temporary, 0700); err != nil {
		return nil, err
	}
	options := url.Values{"threads": {"2"}, "temp_directory": {temporary}}
	initializer := func(conn driver.ExecerContext) error {
		if err := verifyBuiltins(conn, proof); err != nil {
			return err
		}
		_, err := conn.ExecContext(context.Background(), "CONNECT "+literal(descriptor.URI)+" (TOKEN "+literal(descriptor.Token)+")", nil)
		return err
	}
	connector, err := duckdb.NewConnector(":memory:?"+options.Encode(), initializer)
	if err != nil {
		return nil, err
	}
	db := sql.OpenDB(connector)
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	if err = db.Ping(); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}
func run(root string, descriptor state, report *output) error {
	report.Stage = "builtin_extensions_loaded_before_connect"
	start := time.Now()
	db, err := open(root, descriptor, &report.BuiltinExtensions)
	report.Checks = append(report.Checks, check{report.Stage, err == nil, float64(time.Since(start).Microseconds()) / 1000})
	if err != nil {
		return err
	}
	defer db.Close()
	other, err := open(root, descriptor, nil)
	if err != nil {
		return err
	}
	defer other.Close()
	ctx := context.Background()
	table := fmt.Sprintf("codex_native_go_%d", os.Getpid())
	bulk := table + "_bulk"
	defer db.ExecContext(ctx, "DROP TABLE IF EXISTS "+bulk)
	defer db.ExecContext(ctx, "DROP TABLE IF EXISTS "+table)
	gate := func(name string, fn func() error) error {
		report.Stage = name
		start := time.Now()
		err := fn()
		report.Checks = append(report.Checks, check{name, err == nil, float64(time.Since(start).Microseconds()) / 1000})
		return err
	}
	scalar := func(db *sql.DB, q string, args ...any) (int64, error) {
		var n int64
		err := db.QueryRowContext(ctx, q, args...).Scan(&n)
		return n, err
	}
	exec := func(q string, args ...any) error { _, err := db.ExecContext(ctx, q, args...); return err }
	if err = gate("official_driver_native_identity", func() error {
		if err := db.QueryRow("SELECT version()").Scan(&report.NativeVersion); err != nil {
			return err
		}
		report.ClientLibraryVersion = mapping.LibraryVersion()
		report.NativeUsed = report.NativeVersion == "v2.1.0-dev0" && report.ClientLibraryVersion == report.NativeVersion
		return require(report.NativeUsed, "custom native engine was not loaded")
	}); err != nil {
		return err
	}
	if err = gate("ddl_schema_freshness_independent_connections", func() error {
		if err := exec("CREATE TABLE " + table + "(id INTEGER PRIMARY KEY,val BIGINT,label VARCHAR,amount DECIMAL(18,4),raw BLOB,nums INTEGER[],nullable INTEGER,nest STRUCT(label VARCHAR,v BIGINT))"); err != nil {
			return err
		}
		n, err := scalar(other, "SELECT count(*) FROM "+table)
		if err != nil {
			return err
		}
		return require(n == 0, "fresh DDL missing from second connection")
	}); err != nil {
		return err
	}
	if err = gate("positional_typed_list_blob_null_exact_decimal_cast", func() error {
		return exec("INSERT INTO "+table+" VALUES(?,?,?,?,?,?,?,?)", 1, int64(9), "ñ-Go-🙂", "123.4500", []byte{0, 1, 127, 255}, []int32{1, 2, 3}, nil, map[string]any{"label": "struct-Go", "v": int64(42)})
	}); err != nil {
		return err
	}
	if err = gate("native_complex_results_preserve_values", func() error {
		var id int32
		var val int64
		var label string
		var amount duckdb.Decimal
		var raw []byte
		var nums, nest any
		var nullable sql.NullInt64
		if err := other.QueryRow("SELECT * FROM "+table+" WHERE id=?", 1).Scan(&id, &val, &label, &amount, &raw, &nums, &nullable, &nest); err != nil {
			return err
		}
		if err := require(id == 1 && val == 9 && label == "ñ-Go-🙂" && amount.Width == 18 && amount.Scale == 4 && amount.Value.Cmp(big.NewInt(1234500)) == 0 && bytes.Equal(raw, []byte{0, 1, 127, 255}) && !nullable.Valid, "scalar/blob/decimal/null mismatch"); err != nil {
			return err
		}
		if err := require(reflect.DeepEqual(nums, []any{int32(1), int32(2), int32(3)}), fmt.Sprintf("LIST mismatch %T %v", nums, nums)); err != nil {
			return err
		}
		return require(reflect.DeepEqual(nest, map[string]any{"label": "struct-Go", "v": int64(42)}), "STRUCT mismatch")
	}); err != nil {
		return err
	}
	if err = gate("named_repeated_prepared_parameters", func() error {
		stmt, err := db.PrepareContext(ctx, "SELECT $value::BIGINT+$value::BIGINT,$label::VARCHAR")
		if err != nil {
			return err
		}
		defer stmt.Close()
		var n int64
		var text string
		if err := stmt.QueryRowContext(ctx, sql.Named("label", "named"), sql.Named("value", int64(7))).Scan(&n, &text); err != nil {
			return err
		}
		return require(n == 14 && text == "named", "named repeated parameters mismatch")
	}); err != nil {
		return err
	}
	if err = gate("prepared_reuse_null_first_bulk_crud", func() error {
		if err := exec("CREATE TABLE " + bulk + "(id INTEGER PRIMARY KEY,note VARCHAR)"); err != nil {
			return err
		}
		stmt, err := db.PrepareContext(ctx, "INSERT INTO "+bulk+" VALUES(?,?)")
		if err != nil {
			return err
		}
		defer stmt.Close()
		for i := 0; i < 128; i++ {
			var note any
			if i > 0 {
				note = fmt.Sprintf("row-%d", i)
			}
			result, err := stmt.ExecContext(ctx, i, note)
			if err != nil {
				return err
			}
			changed, err := result.RowsAffected()
			if err != nil {
				return err
			}
			if changed != 1 {
				return fmt.Errorf("prepared insert RowsAffected returned %d, expected 1", changed)
			}
		}
		if err := exec("UPDATE "+bulk+" SET note=? WHERE id=?", "updated", 1); err != nil {
			return err
		}
		if err := exec("DELETE FROM "+bulk+" WHERE id=?", 127); err != nil {
			return err
		}
		n, err := scalar(other, "SELECT count(*) FROM "+bulk)
		if err != nil {
			return err
		}
		return require(n == 127, "prepared bulk CRUD count mismatch")
	}); err != nil {
		return err
	}
	if err = gate("native_errors_and_recovery", func() error {
		if err := exec("SELECT ?::DECIMAL(18,4)", duckdb.Decimal{Width: 18, Scale: 4, Value: big.NewInt(1234500)}); err == nil {
			return errors.New("official direct Decimal limitation unexpectedly changed")
		}
		if err := exec("INSERT INTO " + bulk + " VALUES(0,'duplicate')"); err == nil {
			return errors.New("duplicate primary key accepted")
		} else {
			var engineError *duckdb.Error
			if !errors.As(err, &engineError) || engineError.Type != duckdb.ErrorTypeConstraint {
				return fmt.Errorf("constraint error class mismatch: %v", err)
			}
		}
		if err := exec("SELECT FROM"); err == nil {
			return errors.New("invalid SQL accepted")
		}
		n, err := scalar(db, "SELECT ?::BIGINT", 41)
		if err != nil {
			return err
		}
		return require(n == 41, "query errors prevented recovery")
	}); err != nil {
		return err
	}
	if err = gate("iterative_result_paging_15003_rows", func() error {
		rows, err := db.QueryContext(ctx, "SELECT i::BIGINT AS value FROM range(15003) t(i)")
		if err != nil {
			return err
		}
		defer rows.Close()
		columns, err := rows.Columns()
		if err != nil {
			return err
		}
		if err := require(reflect.DeepEqual(columns, []string{"value"}), "column metadata mismatch"); err != nil {
			return err
		}
		var count int64
		for rows.Next() {
			var v int64
			if err := rows.Scan(&v); err != nil {
				return err
			}
			if v != count {
				return errors.New("paged value mismatch")
			}
			count++
		}
		if err := rows.Err(); err != nil {
			return err
		}
		return require(count == 15003, "paged result truncated")
	}); err != nil {
		return err
	}
	if err = gate("transactions_snapshot_nonblocking_reader_commit_rollback", func() error {
		reader, err := other.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer reader.Rollback()
		var n int64
		if err := reader.QueryRow("SELECT val FROM " + table + " WHERE id=1").Scan(&n); err != nil {
			return err
		}
		if err := require(n == 9, "initial snapshot mismatch"); err != nil {
			return err
		}
		writer, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer writer.Rollback()
		if _, err := writer.Exec("UPDATE " + table + " SET val=10 WHERE id=1"); err != nil {
			return err
		}
		started := time.Now()
		if err := reader.QueryRow("SELECT val FROM " + table + " WHERE id=1").Scan(&n); err != nil {
			return err
		}
		if err := require(n == 9 && time.Since(started) < 2*time.Second, "reader blocked or saw uncommitted value"); err != nil {
			return err
		}
		if err := writer.Commit(); err != nil {
			return err
		}
		if err := reader.QueryRow("SELECT val FROM " + table + " WHERE id=1").Scan(&n); err != nil {
			return err
		}
		if err := require(n == 9, "snapshot changed after other writer committed"); err != nil {
			return err
		}
		if err := reader.Commit(); err != nil {
			return err
		}
		n, err = scalar(other, "SELECT val FROM "+table+" WHERE id=1")
		if err != nil {
			return err
		}
		if err := require(n == 10, "committed value missing"); err != nil {
			return err
		}
		rollback, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer rollback.Rollback()
		if _, err := rollback.Exec("UPDATE " + table + " SET val=999 WHERE id=1"); err != nil {
			return err
		}
		if err := rollback.Rollback(); err != nil {
			return err
		}
		n, err = scalar(other, "SELECT val FROM "+table+" WHERE id=1")
		if err != nil {
			return err
		}
		return require(n == 10, "rollback leaked mutation")
	}); err != nil {
		return err
	}
	if err = gate("mvcc_conflicting_writers_recover", func() error {
		first, err := db.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer first.Rollback()
		second, err := other.BeginTx(ctx, nil)
		if err != nil {
			return err
		}
		defer second.Rollback()
		var initial int64
		if err := first.QueryRow("SELECT val FROM " + table + " WHERE id=1").Scan(&initial); err != nil {
			return err
		}
		if err := second.QueryRow("SELECT val FROM " + table + " WHERE id=1").Scan(&initial); err != nil {
			return err
		}
		if _, err := first.Exec("UPDATE " + table + " SET val=11 WHERE id=1"); err != nil {
			return err
		}
		if _, err := second.Exec("UPDATE " + table + " SET val=12 WHERE id=1"); err == nil {
			return errors.New("conflicting writer unexpectedly succeeded")
		} else {
			var nativeError *duckdb.Error
			if !errors.As(err, &nativeError) || nativeError.Type != duckdb.ErrorTypeTransaction {
				return fmt.Errorf("conflict native error type mismatch: %v", err)
			}
		}
		if err := second.Rollback(); err != nil {
			return err
		}
		if err := first.Commit(); err != nil {
			return err
		}
		n, err := scalar(other, "SELECT val FROM "+table+" WHERE id=1")
		if err != nil {
			return err
		}
		if err := require(n == 11, "winning writer state lost"); err != nil {
			return err
		}
		return exec("UPDATE " + table + " SET val=10 WHERE id=1")
	}); err != nil {
		return err
	}
	if err = gate("connection_close_rolls_back_pending_transaction", func() error {
		pending, err := open(root, descriptor, nil)
		if err != nil {
			return err
		}
		if _, err = pending.Exec("BEGIN"); err != nil {
			pending.Close()
			return err
		}
		if _, err = pending.Exec("UPDATE " + table + " SET val=666 WHERE id=1"); err != nil {
			pending.Close()
			return err
		}
		if err = pending.Close(); err != nil {
			return err
		}
		n, err := scalar(other, "SELECT val FROM "+table+" WHERE id=1")
		if err != nil {
			return err
		}
		return require(n == 10, "closed connection retained transaction")
	}); err != nil {
		return err
	}
	exists, err := scalar(db, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'")
	if err != nil {
		return err
	}
	if exists > 0 {
		if err = gate("multiclient_shared_table", func() error {
			rows, err := db.Query("SELECT id,label FROM lang_multiclient_probe ORDER BY id")
			if err != nil {
				return err
			}
			for rows.Next() {
				var row sharedRow
				if err := rows.Scan(&row.ID, &row.Label); err != nil {
					rows.Close()
					return err
				}
				report.RowsSeen = append(report.RowsSeen, row)
			}
			err = rows.Err()
			rows.Close()
			if err != nil {
				return err
			}
			if err := require(len(report.RowsSeen) > 0, "previous language rows missing"); err != nil {
				return err
			}
			if err := exec("INSERT INTO lang_multiclient_probe VALUES(?,?)", 4, "from Go"); err != nil {
				return err
			}
			n, err := scalar(other, "SELECT count(*) FROM lang_multiclient_probe WHERE id=? AND label=?", 4, "from Go")
			if err != nil {
				return err
			}
			report.Interop = n == 1
			return require(report.Interop, "Go row missing on independent connection")
		}); err != nil {
			return err
		}
	}
	report.Stage = ""
	report.Passed = true
	return nil
}
func main() {
	report := output{Driver: "github.com/duckdb/duckdb-go/v2@v2.10506.0", Checks: []check{}, Limits: []string{"Official driver rejects direct duckdb.Decimal parameters; exact decimal input uses a bound string converted by DuckDB to DECIMAL(18,4).", "Validated only on macOS arm64 with the pinned native engine and Quack protocol 7."}}
	var descriptor state
	err := func() error {
		if len(os.Args) != 3 {
			return errors.New("usage: native-go-client ROOT STATE_JSON")
		}
		root, err := filepath.Abs(os.Args[1])
		if err != nil {
			return err
		}
		data, err := os.ReadFile(os.Args[2])
		if err != nil {
			return err
		}
		if err = json.Unmarshal(data, &descriptor); err != nil {
			return err
		}
		if descriptor.URI == "" {
			descriptor.URI = descriptor.Endpoint
		}
		if descriptor.URI == "" || descriptor.Token == "" {
			return errors.New("incomplete private native descriptor")
		}
		return run(root, descriptor, &report)
	}()
	if err != nil {
		report.Error = err.Error()
		if descriptor.Token != "" {
			report.Error = strings.ReplaceAll(report.Error, descriptor.Token, "[REDACTED]")
		}
	}
	json.NewEncoder(os.Stdout).Encode(report)
	if err != nil {
		os.Exit(1)
	}
}

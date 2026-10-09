-- LuaJIT FFI calls DuckDB's native C API directly; no SQL transport adapter.
-- Usage: luajit examples/lua/acceptance.lua [ROOT PRIVATE_STATE_JSON]
-- Optional DUCKONWIRE_LIBRARY selects the paired native shared library.
local ffi = require('ffi')
ffi.cdef[[
typedef uint64_t idx_t;
typedef void *duckdb_database;
typedef void *duckdb_connection;
typedef void *duckdb_prepared_statement;
typedef void *duckdb_logical_type;
typedef void *duckdb_value;
typedef void *duckdb_data_chunk;
typedef void *duckdb_vector;
typedef struct { uint64_t lower; int64_t upper; } duckdb_hugeint;
typedef struct { uint8_t width; uint8_t scale; duckdb_hugeint value; } duckdb_decimal;
typedef struct { void *data; idx_t size; } duckdb_blob;
typedef struct {
  idx_t deprecated_column_count, deprecated_row_count, deprecated_rows_changed;
  void *deprecated_columns; char *deprecated_error_message; void *internal_data;
} duckdb_result;
int duckdb_open(const char *, duckdb_database *);
void duckdb_close(duckdb_database *);
int duckdb_connect(duckdb_database, duckdb_connection *);
void duckdb_disconnect(duckdb_connection *);
const char *duckdb_library_version(void);
int duckdb_query(duckdb_connection, const char *, duckdb_result *);
void duckdb_destroy_result(duckdb_result *);
int duckdb_result_error_type(duckdb_result *);
int duckdb_result_return_type(duckdb_result);
idx_t duckdb_rows_changed(duckdb_result *);
idx_t duckdb_row_count(duckdb_result *);
idx_t duckdb_column_count(duckdb_result *);
const char *duckdb_column_name(duckdb_result *, idx_t);
int duckdb_column_type(duckdb_result *, idx_t);
int64_t duckdb_value_int64(duckdb_result *, idx_t, idx_t);
bool duckdb_value_boolean(duckdb_result *, idx_t, idx_t);
char *duckdb_value_varchar(duckdb_result *, idx_t, idx_t);
duckdb_blob duckdb_value_blob(duckdb_result *, idx_t, idx_t);
bool duckdb_value_is_null(duckdb_result *, idx_t, idx_t);
void duckdb_free(void *);
int duckdb_prepare(duckdb_connection, const char *, duckdb_prepared_statement *);
void duckdb_destroy_prepare(duckdb_prepared_statement *);
int duckdb_bind_parameter_index(duckdb_prepared_statement, idx_t *, const char *);
int duckdb_clear_bindings(duckdb_prepared_statement);
int duckdb_bind_int32(duckdb_prepared_statement, idx_t, int32_t);
int duckdb_bind_int64(duckdb_prepared_statement, idx_t, int64_t);
int duckdb_bind_varchar(duckdb_prepared_statement, idx_t, const char *);
int duckdb_bind_varchar_length(duckdb_prepared_statement, idx_t, const char *, idx_t);
int duckdb_bind_blob(duckdb_prepared_statement, idx_t, const void *, idx_t);
int duckdb_bind_decimal(duckdb_prepared_statement, idx_t, duckdb_decimal);
int duckdb_bind_null(duckdb_prepared_statement, idx_t);
int duckdb_bind_value(duckdb_prepared_statement, idx_t, duckdb_value);
int duckdb_execute_prepared(duckdb_prepared_statement, duckdb_result *);
int duckdb_execute_prepared_streaming(duckdb_prepared_statement, duckdb_result *);
duckdb_logical_type duckdb_create_logical_type(int);
void duckdb_destroy_logical_type(duckdb_logical_type *);
duckdb_value duckdb_create_int32(int32_t);
duckdb_value duckdb_create_list_value(duckdb_logical_type, duckdb_value *, idx_t);
void duckdb_destroy_value(duckdb_value *);
duckdb_data_chunk duckdb_fetch_chunk(duckdb_result);
idx_t duckdb_data_chunk_get_size(duckdb_data_chunk);
duckdb_vector duckdb_data_chunk_get_vector(duckdb_data_chunk, idx_t);
void *duckdb_vector_get_data(duckdb_vector);
void duckdb_destroy_data_chunk(duckdb_data_chunk *);
int getpid(void);
]]

local source = debug.getinfo(1,'S').source
if source:sub(1,1) == '@' then source = source:sub(2) end
local directory = source:match('^(.*)/[^/]+$') or '.'
local root = arg[1] or directory .. '/../..'
local state_path = arg[2] or root .. '/.runtime/native.json'
local function exists(path)
  local file = io.open(path, 'rb')
  if file then file:close(); return true end
  return false
end
local library_path = os.getenv('DUCKONWIRE_LIBRARY')
if not library_path or library_path == '' then
  local filename = ffi.os == 'OSX' and 'libduckdb.dylib' or 'libduckdb.so'
  for _,candidate in ipairs({root .. '/lib/' .. filename, root .. '/build/lib/' .. filename}) do
    if exists(candidate) then library_path = candidate; break end
  end
end
local ok_load, db = pcall(ffi.load, library_path or '')
local report = {language='Lua', driver='LuaJIT FFI direct official DuckDB C API',
  runtime=jit.version, native_used=true, custom_protocol_wrapper=false,
  manual_load_used=false, passed=false, checks={}}
local stage = 'usage_ROOT_STATE_JSON'
local database = ffi.new('duckdb_database[1]')
local connections, statements = {}, {}
local table_name = 'codex_native_lua_' .. tonumber(ffi.C.getpid())
local created = false
local function require_ok(condition, label)
  stage = label or stage
  if not condition then error(stage, 0) end
end
local function native_ok(value, label) require_ok(value == 0, label) end
local function quote(value) return "'" .. value:gsub("'", "''") .. "'" end
local function text(result, column, row)
  local value = db.duckdb_value_varchar(result, column, row or 0)
  if value == nil then return nil end
  local output = ffi.string(value)
  db.duckdb_free(value)
  return output
end
local function run_result(executor, label, inspect)
  stage = label
  local result = ffi.new('duckdb_result[1]')
  local native_status = executor(result)
  if native_status ~= 0 then
    report.error_type = tonumber(db.duckdb_result_error_type(result))
    db.duckdb_destroy_result(result)
    error(label, 0)
  end
  local successful, output = pcall(inspect or function() end, result)
  db.duckdb_destroy_result(result)
  if not successful then error(output, 0) end
  return output
end
local function query(con, sql, label, inspect)
  return run_result(function(result) return db.duckdb_query(con[0], sql, result) end,
                    label, inspect)
end
local function scalar(con, sql, label)
  return query(con, sql, label, function(result)
    require_ok(tonumber(db.duckdb_row_count(result)) == 1, label .. '_one_row')
    return tonumber(db.duckdb_value_int64(result, 0, 0))
  end)
end
local function connection()
  local con = ffi.new('duckdb_connection[1]')
  native_ok(db.duckdb_connect(database[0], con), 'native_connection')
  connections[#connections+1] = con
  return con
end
local function prepare(con, sql, label)
  local prepared = ffi.new('duckdb_prepared_statement[1]')
  statements[#statements+1] = prepared
  native_ok(db.duckdb_prepare(con[0], sql, prepared), label)
  return prepared
end
local function execute(prepared, label, inspect)
  return run_result(function(result) return db.duckdb_execute_prepared(prepared[0], result) end,
                    label, inspect)
end
local function rows_changed(result, expected)
  require_ok(tonumber(db.duckdb_rows_changed(result)) == expected, 'native_changed_rows_count')
  require_ok(db.duckdb_result_return_type(result[0]) == 1, 'native_changed_rows_metadata')
end
local function check(name, work)
  local started = os.clock()
  stage = name
  work()
  report.checks[#report.checks+1] = {name=name, passed=true, cpu_seconds=os.clock()-started}
end

local function main()
  require_ok(root and state_path, 'usage_ROOT_STATE_JSON')
  require_ok(ok_load, 'load_native_shared_engine')
  report.engine_version = ffi.string(db.duckdb_library_version())
  require_ok(report.engine_version == 'v2.1.0-dev0', 'matching_native_engine_version')
  native_ok(db.duckdb_open(nil, database), 'native_open')
  local a, b = connection(), connection()
  check('builtin_extensions_loaded_without_LOAD_and_unsigned_flags', function()
    require_ok(scalar(a, [[SELECT count(*) FROM duckdb_extensions()
      WHERE extension_name IN ('httpfs','quack') AND loaded AND installed
      AND install_mode='STATICALLY_LINKED' AND install_path='(BUILT-IN)']], 'builtin_metadata') == 2,
      'two_default_builtin_extensions')
    require_ok(scalar(a, "SELECT current_setting('allow_unsigned_extensions')::INTEGER", 'default_unsigned') == 0,
      'unsigned_extensions_disabled')
    report.builtin_extensions = {'httpfs', 'quack'}
    report.allow_unsigned_extensions = false
  end)
  local endpoint, token = query(a, 'SELECT endpoint,token FROM read_json_auto(' .. quote(state_path) .. ')',
    'private_descriptor', function(result)
      require_ok(tonumber(db.duckdb_row_count(result)) == 1, 'private_descriptor_one_row')
      return {text(result, 0), text(result, 1)}
    end), nil
  endpoint, token = endpoint[1], endpoint[2]
  require_ok(endpoint and token, 'private_descriptor_fields')
  local function connect(con)
    query(con, 'CONNECT ' .. quote(endpoint) .. ' (TOKEN ' .. quote(token) .. ')', 'native_CONNECT')
  end
  connect(a); connect(b)
  query(a, 'CREATE TABLE ' .. table_name ..
    '(id BIGINT PRIMARY KEY,label VARCHAR,amount DECIMAL(18,2),payload BLOB,items INTEGER[],optional INTEGER)', 'create_table')
  created = true
  local insert = prepare(a, 'INSERT INTO ' .. table_name .. ' VALUES (?,?,?,?,?,?)', 'prepare_typed_insert')
  local function bind_row(id, label, optional)
    native_ok(db.duckdb_clear_bindings(insert[0]), 'clear_prepared_bindings')
    native_ok(db.duckdb_bind_int64(insert[0], 1, id), 'bind_bigint')
    native_ok(db.duckdb_bind_varchar(insert[0], 2, label), 'bind_unicode')
    local decimal = ffi.new('duckdb_decimal', {18, 2, {1234567, 0}})
    native_ok(db.duckdb_bind_decimal(insert[0], 3, decimal), 'bind_exact_decimal')
    native_ok(db.duckdb_bind_blob(insert[0], 4, '\0A\255B', 4), 'bind_binary_blob')
    local kind = ffi.new('duckdb_logical_type[1]', db.duckdb_create_logical_type(4))
    local values = ffi.new('duckdb_value[3]')
    for i=0,2 do values[i] = db.duckdb_create_int32(i+1) end
    local list = ffi.new('duckdb_value[1]', db.duckdb_create_list_value(kind[0], values, 3))
    local binding = db.duckdb_bind_value(insert[0], 5, list[0])
    db.duckdb_destroy_value(list)
    for i=0,2 do
      local value = ffi.new('duckdb_value[1]', values[i]); db.duckdb_destroy_value(value)
    end
    db.duckdb_destroy_logical_type(kind)
    native_ok(binding, 'bind_native_list')
    if optional == nil then native_ok(db.duckdb_bind_null(insert[0], 6), 'bind_null')
    else native_ok(db.duckdb_bind_int32(insert[0], 6, optional), 'bind_reused_integer') end
  end
  check('typed_prepared_CRUD_decimal_blob_list_null_unicode_and_reuse', function()
    bind_row(1, "niño ' Lua", nil)
    execute(insert, 'insert_typed_null', function(result) rows_changed(result, 1) end)
    bind_row(2, 'reused', 42)
    execute(insert, 'insert_reused_integer', function(result) rows_changed(result, 1) end)
    query(b, 'SELECT id,label,amount,payload,items,optional,items[1],items[2],items[3] FROM ' .. table_name .. ' ORDER BY id', 'read_typed_rows', function(result)
      require_ok(tonumber(db.duckdb_row_count(result)) == 2, 'typed_two_rows')
      report.typed_values = {text(result, 1), text(result, 2)}
      require_ok(report.typed_values[1] == "niño ' Lua", 'typed_unicode_value')
      require_ok(report.typed_values[2] == '12345.67', 'typed_decimal_value')
      require_ok(db.duckdb_column_type(result, 4) == 24, 'original_native_LIST_result_type')
      -- Legacy scalar C getters do not convert LIST values to VARCHAR. Verify
      -- its native result type and exact indexed elements instead.
      report.typed_list = {}
      for index=1,3 do
        report.typed_list[index] = tonumber(db.duckdb_value_int64(result, index+5, 0))
        require_ok(report.typed_list[index] == index, 'typed_list_element_' .. index)
      end
      local blob = db.duckdb_value_blob(result, 3, 0)
      local data = ffi.string(blob.data, tonumber(blob.size)); db.duckdb_free(blob.data)
      require_ok(data == '\0A\255B', 'exact_blob_bytes')
      require_ok(db.duckdb_value_is_null(result, 5, 0) and tonumber(db.duckdb_value_int64(result, 5, 1)) == 42, 'null_to_integer_reuse')
    end)
    local update = prepare(a, 'UPDATE ' .. table_name .. ' SET optional=? WHERE id=?', 'prepare_update')
    native_ok(db.duckdb_bind_int32(update[0], 1, 43), 'bind_update_value')
    native_ok(db.duckdb_bind_int64(update[0], 2, 2), 'bind_update_id')
    execute(update, 'prepared_update', function(result) rows_changed(result, 1) end)
  end)
  check('native_named_bindings_repeated_case_and_exact_bigint', function()
    local named = prepare(a, 'SELECT $value::BIGINT AS value,$VALUE::BIGINT AS value', 'prepare_named')
    local index = ffi.new('idx_t[1]')
    native_ok(db.duckdb_bind_parameter_index(named[0], index, 'value'), 'native_named_parameter_index')
    native_ok(db.duckdb_bind_int64(named[0], index[0], 9007199254740993LL), 'bind_exact_bigint')
    execute(named, 'execute_named', function(result)
      require_ok(text(result, 0) == '9007199254740993' and text(result, 1) == '9007199254740993', 'exact_bigint_result')
      require_ok(ffi.string(db.duckdb_column_name(result, 0)) == 'value' and ffi.string(db.duckdb_column_name(result, 1)) == 'value', 'duplicate_native_result_names')
    end)
  end)
  check('two_native_sessions_isolation_commit_and_rollback', function()
    query(a, 'BEGIN', 'begin_transaction')
    bind_row(3, 'uncommitted', nil); execute(insert, 'insert_uncommitted')
    require_ok(scalar(a, 'SELECT count(*) FROM ' .. table_name, 'own_uncommitted') == 3, 'own_write_visible')
    require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'other_uncommitted') == 2, 'uncommitted_hidden')
    query(a, 'COMMIT', 'commit_transaction')
    require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'committed_count') == 3, 'committed_visible')
    query(a, 'BEGIN', 'begin_rollback')
    bind_row(4, 'rollback', nil); execute(insert, 'insert_rollback')
    query(a, 'ROLLBACK', 'rollback_transaction')
    require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'rolled_back_count') == 3, 'rollback_hidden')
  end)
  check('MVCC_reader_snapshot_progresses_through_32_writer_commits', function()
    query(b, 'BEGIN', 'begin_reader_snapshot')
    require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'initial_snapshot_count') == 3, 'initial_snapshot')
    for i=10,41 do
      query(a, 'BEGIN', 'begin_writer_batch')
      bind_row(i, 'concurrent', i); execute(insert, 'write_while_reader_open')
      query(a, 'COMMIT', 'commit_writer_batch')
      require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'snapshot_reader_count') == 3, 'snapshot_stays_consistent')
    end
    query(b, 'COMMIT', 'commit_reader_snapshot')
    require_ok(scalar(b, 'SELECT count(*) FROM ' .. table_name, 'fresh_reader_count') == 35, 'all_writer_commits_visible')
    report.concurrent_commits = 32
  end)
  check('native_constraint_error_type_and_connection_recovery', function()
    bind_row(1, 'duplicate', nil)
    local result = ffi.new('duckdb_result[1]')
    local status = db.duckdb_execute_prepared(insert[0], result)
    local error_type = tonumber(db.duckdb_result_error_type(result))
    db.duckdb_destroy_result(result)
    require_ok(status ~= 0 and error_type == 18, 'native_constraint_error')
    require_ok(scalar(a, 'SELECT 42', 'query_after_native_error') == 42, 'native_error_recovery')
    report.constraint_error_type = error_type
  end)
  check('complex_CTE_window_aggregate_and_native_chunk_paging', function()
    require_ok(scalar(b, 'WITH ranked AS (SELECT id,row_number() OVER (ORDER BY id) AS rn FROM ' .. table_name .. ') SELECT sum(rn) FROM ranked', 'complex_window_CTE') == 630, 'complex_window_exact_result')
    local prepared = prepare(b, 'SELECT i::BIGINT FROM range(15003) t(i)', 'prepare_native_stream')
    run_result(function(result) return db.duckdb_execute_prepared_streaming(prepared[0], result) end,
      'execute_native_stream', function(result)
        local rows, chunks = 0, 0
        while true do
          local chunk = ffi.new('duckdb_data_chunk[1]', db.duckdb_fetch_chunk(result[0]))
          if chunk[0] == nil then break end
          local count = tonumber(db.duckdb_data_chunk_get_size(chunk[0]))
          local vector = db.duckdb_data_chunk_get_vector(chunk[0], 0)
          local values = ffi.cast('int64_t *', db.duckdb_vector_get_data(vector))
          local ordered = true
          for i=0,count-1 do if tonumber(values[i]) ~= rows+i then ordered = false; break end end
          db.duckdb_destroy_data_chunk(chunk)
          require_ok(ordered, 'native_chunk_values_in_order')
          rows, chunks = rows+count, chunks+1
          if chunks == 1 then
            bind_row(100, 'during paging', nil); execute(insert, 'write_during_native_chunk_paging')
          end
        end
        require_ok(rows == 15003 and chunks > 1, 'all_native_stream_rows_received')
        report.stream_rows, report.stream_chunks = rows, chunks
      end)
  end)
  check('native_disconnect_rolls_back_abandoned_transaction', function()
    local abandoned = connection(); connect(abandoned)
    query(abandoned, 'BEGIN', 'begin_abandoned')
    local add = prepare(abandoned, 'INSERT INTO ' .. table_name .. '(id,label) VALUES (?,?)', 'prepare_abandoned')
    native_ok(db.duckdb_bind_int64(add[0], 1, 999), 'bind_abandoned_id')
    native_ok(db.duckdb_bind_varchar(add[0], 2, 'abandoned'), 'bind_abandoned_label')
    execute(add, 'insert_abandoned')
    db.duckdb_destroy_prepare(add)
    db.duckdb_disconnect(abandoned)
    require_ok(scalar(a, 'SELECT count(*) FROM ' .. table_name .. ' WHERE id=999', 'read_after_disconnect') == 0, 'disconnect_rollback')
  end)
  check('prepared_delete_affected_rows_and_shared_language_marker', function()
    local remove = prepare(a, 'DELETE FROM ' .. table_name .. ' WHERE id>=?', 'prepare_delete')
    native_ok(db.duckdb_bind_int64(remove[0], 1, 10), 'bind_delete_floor')
    execute(remove, 'execute_delete', function(result) rows_changed(result, 33) end)
    if scalar(a, "SELECT count(*) FROM information_schema.tables WHERE table_name='lang_multiclient_probe'", 'shared_table_exists') == 1 then
      query(a, 'SELECT label FROM lang_multiclient_probe WHERE id=0', 'read_python_shared_marker', function(result)
        require_ok(text(result, 0) == 'from Python', 'python_marker_value')
      end)
      local add = prepare(a, 'INSERT INTO lang_multiclient_probe VALUES (?,?)', 'prepare_shared_marker')
      native_ok(db.duckdb_bind_int32(add[0], 1, 9), 'bind_shared_id')
      native_ok(db.duckdb_bind_varchar(add[0], 2, 'from Lua'), 'bind_shared_label')
      execute(add, 'insert_shared_marker', function(result) rows_changed(result, 1) end)
      report.shared_language_marker = true
    end
  end)
  query(a, 'DROP TABLE ' .. table_name, 'cleanup_owned_table')
  created = false
  report.passed = true
end

local completed, failure = pcall(main)
if not completed then report.failure_stage = stage; report.passed = false end
if ok_load then
  for _, prepared in ipairs(statements) do db.duckdb_destroy_prepare(prepared) end
  if created and connections[1] and connections[1][0] ~= nil then
    local result = ffi.new('duckdb_result[1]')
    db.duckdb_query(connections[1][0], 'ROLLBACK', result); db.duckdb_destroy_result(result)
    db.duckdb_query(connections[1][0], 'DROP TABLE IF EXISTS ' .. table_name, result); db.duckdb_destroy_result(result)
  end
  for _, con in ipairs(connections) do db.duckdb_disconnect(con) end
  db.duckdb_close(database)
end
local function json(value)
  if type(value) == 'string' then
    return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
      return string.format('\\u%04x', c:byte())
    end) .. '"'
  elseif type(value) == 'boolean' or type(value) == 'number' then return tostring(value)
  elseif type(value) == 'table' then
    local parts = {}
    if #value > 0 then
      for _, item in ipairs(value) do parts[#parts+1] = json(item) end
      return '[' .. table.concat(parts, ',') .. ']'
    end
    for key, item in pairs(value) do parts[#parts+1] = json(key) .. ':' .. json(item) end
    table.sort(parts)
    return '{' .. table.concat(parts, ',') .. '}'
  end
  return 'null'
end
print(json(report))
os.exit(report.passed and 0 or 1)

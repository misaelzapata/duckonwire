-- Plain CRUD through LuaJIT FFI and DuckDB's official C API.
-- Usage: luajit examples/lua/client.lua
-- Connection settings: DUCKONWIRE_ENDPOINT and DUCKONWIRE_TOKEN.
-- Optional DUCKONWIRE_LIBRARY selects the paired native shared library.
local ffi = require('ffi')
ffi.cdef[[
typedef uint64_t idx_t;
typedef void *duckdb_database;
typedef void *duckdb_connection;
typedef void *duckdb_prepared_statement;
typedef struct {
  idx_t deprecated_column_count,deprecated_row_count,deprecated_rows_changed;
  void *deprecated_columns; char *deprecated_error_message; void *internal_data;
} duckdb_result;
int duckdb_open(const char *,duckdb_database *);
void duckdb_close(duckdb_database *);
const char *duckdb_library_version(void);
int duckdb_connect(duckdb_database,duckdb_connection *);
void duckdb_disconnect(duckdb_connection *);
int duckdb_query(duckdb_connection,const char *,duckdb_result *);
void duckdb_destroy_result(duckdb_result *);
idx_t duckdb_row_count(duckdb_result *);
char *duckdb_value_varchar(duckdb_result *,idx_t,idx_t);
int64_t duckdb_value_int64(duckdb_result *,idx_t,idx_t);
void duckdb_free(void *);
int duckdb_prepare(duckdb_connection,const char *,duckdb_prepared_statement *);
void duckdb_destroy_prepare(duckdb_prepared_statement *);
int duckdb_bind_int32(duckdb_prepared_statement,idx_t,int32_t);
int duckdb_bind_varchar(duckdb_prepared_statement,idx_t,const char *);
int duckdb_execute_prepared(duckdb_prepared_statement,duckdb_result *);
]]
local source = debug.getinfo(1,'S').source
if source:sub(1,1) == '@' then source = source:sub(2) end
local directory = source:match('^(.*)/[^/]+$') or '.'
local root = directory .. '/../..'
local endpoint = os.getenv('DUCKONWIRE_ENDPOINT') or 'quack:127.0.0.1:9495'
local token = assert(os.getenv('DUCKONWIRE_TOKEN'), 'Set DUCKONWIRE_TOKEN to the server token')
assert(token ~= '', 'Set DUCKONWIRE_TOKEN to the server token')
local library = os.getenv('DUCKONWIRE_LIBRARY')
if not library or library == '' then
  local filename = ffi.os == 'OSX' and 'libduckdb.dylib' or 'libduckdb.so'
  for _,candidate in ipairs({root .. '/lib/' .. filename, root .. '/build/lib/' .. filename}) do
    local file = io.open(candidate,'rb')
    if file then file:close(); library = candidate; break end
  end
end
assert(library and library ~= '', 'Build the native engine or set DUCKONWIRE_LIBRARY to its paired shared library')
local db = ffi.load(library)
assert(ffi.string(db.duckdb_library_version()) == 'v2.1.0-dev0',
  'Use the paired native DuckDB v2.1.0-dev0 shared library')
local database,con = ffi.new('duckdb_database[1]'),ffi.new('duckdb_connection[1]')
local quote = function(value) return "'" .. value:gsub("'","''") .. "'" end
local function varchar(result,column,row)
  local pointer = db.duckdb_value_varchar(result,column,row or 0)
  local value = ffi.string(pointer); db.duckdb_free(pointer); return value
end
local function query(sql,inspect)
  local result = ffi.new('duckdb_result[1]')
  local status = db.duckdb_query(con[0],sql,result)
  local ok,value = pcall(function() assert(status == 0); if inspect then return inspect(result) end end)
  db.duckdb_destroy_result(result)
  assert(ok); return value
end
local function execute(sql,values)
  local statement = ffi.new('duckdb_prepared_statement[1]')
  local result = ffi.new('duckdb_result[1]')
  local ok = pcall(function()
    assert(db.duckdb_prepare(con[0],sql,statement) == 0)
    for index,value in ipairs(values) do
      if type(value) == 'number' then assert(db.duckdb_bind_int32(statement[0],index,value) == 0)
      else assert(db.duckdb_bind_varchar(statement[0],index,value) == 0) end
    end
    assert(db.duckdb_execute_prepared(statement[0],result) == 0)
  end)
  db.duckdb_destroy_result(result); db.duckdb_destroy_prepare(statement)
  assert(ok)
end
local function rows()
  local statement = ffi.new('duckdb_prepared_statement[1]')
  local result = ffi.new('duckdb_result[1]')
  local ok,output = pcall(function()
    assert(db.duckdb_prepare(con[0],
      'SELECT id,name,price::VARCHAR,stock FROM demo_products WHERE client=? ORDER BY id',statement) == 0)
    assert(db.duckdb_bind_varchar(statement[0],1,'Lua') == 0)
    assert(db.duckdb_execute_prepared(statement[0],result) == 0)
    local values = {}
    for row=0,tonumber(db.duckdb_row_count(result))-1 do
      values[#values+1] = {tonumber(db.duckdb_value_int64(result,0,row)),varchar(result,1,row),
        varchar(result,2,row),tonumber(db.duckdb_value_int64(result,3,row))}
    end
    return values
  end)
  db.duckdb_destroy_result(result); db.duckdb_destroy_prepare(statement)
  assert(ok); return output
end
local passed,output = pcall(function()
  assert(db.duckdb_open(nil,database) == 0 and db.duckdb_connect(database[0],con) == 0)
  -- The native engine already includes Quack and httpfs. No LOAD is needed.
  query('CONNECT ' .. quote(endpoint) .. ' (TOKEN ' .. quote(token) .. ')')
  query([[CREATE TABLE IF NOT EXISTS demo_products(client VARCHAR,id INTEGER,name VARCHAR,
    price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))]])
  query('BEGIN')
  execute('DELETE FROM demo_products WHERE client=?',{'Lua'})
  execute('INSERT INTO demo_products VALUES (?,?,?,CAST(? AS DECIMAL(18,2)),?)',{'Lua',1,'Cafe','19.99',3})
  execute('INSERT INTO demo_products VALUES (?,?,?,CAST(? AS DECIMAL(18,2)),?)',{'Lua',2,'Arroz','8.50',10})
  execute('UPDATE demo_products SET stock=stock+1 WHERE client=? AND id=?',{'Lua',1})
  rows() -- Both inserted rows are readable before deleting Arroz.
  execute('DELETE FROM demo_products WHERE client=? AND id=?',{'Lua',2})
  query('COMMIT')
  return rows()
end)
db.duckdb_disconnect(con); db.duckdb_close(database)
if passed then
  -- Fixed demonstration names keep the example free of JSON dependencies.
  local values = {}
  for _,row in ipairs(output) do
    values[#values+1] = string.format('[%d,"%s","%s",%d]',row[1],row[2],row[3],row[4])
  end
  print('{"passed":true,"language":"Lua","rows":[' .. table.concat(values,',') .. ']}')
else
  print('{"passed":false,"language":"Lua","error":"native CRUD failed"}')
  os.exit(1)
end

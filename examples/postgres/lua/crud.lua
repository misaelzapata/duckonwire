local ffi = require('ffi')
ffi.cdef[[
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
PGconn *PQconnectdbParams(const char *const *,const char *const *,int);
int PQstatus(const PGconn *);
char *PQerrorMessage(const PGconn *);
void PQfinish(PGconn *);
PGresult *PQexec(PGconn *,const char *);
PGresult *PQexecParams(PGconn *,const char *,int,const unsigned int *,const char *const *,const int *,const int *,int);
int PQresultStatus(const PGresult *);
char *PQresultErrorMessage(const PGresult *);
int PQntuples(const PGresult *);
char *PQgetvalue(const PGresult *,int,int);
void PQclear(PGresult *);
]]
local pq = ffi.load(os.getenv('PGCLIENT_LIBRARY') or 'pq')
local keys = ffi.new('const char *[8]', {'host','port','user','dbname','password','sslmode','connect_timeout',nil})
local sslmode=os.getenv('PGSSLMODE') or 'disable'
assert(sslmode=='disable' or sslmode=='verify-full','Use PGSSLMODE=disable or verify-full')
assert(sslmode~='verify-full' or os.getenv('PGSSLROOTCERT'),'Set PGSSLROOTCERT for verify-full')
local settings = {os.getenv('PGHOST') or '127.0.0.1',os.getenv('PGPORT') or '9496',os.getenv('PGUSER') or 'duckonwire',os.getenv('PGDATABASE') or 'duckonwire',assert(os.getenv('PGPASSWORD') or os.getenv('DUCKONWIRE_TOKEN'),'Set PGPASSWORD'),sslmode,'5'}
local values = ffi.new('const char *[8]')
for i,value in ipairs(settings) do values[i-1] = value end
local connection = pq.PQconnectdbParams(keys,values,0)
assert(pq.PQstatus(connection) == 0, ffi.string(pq.PQerrorMessage(connection)))
local function query(sql,parameters)
  local result
  if parameters then
    local bound = ffi.new('const char *[?]',#parameters)
    local text = {}
    for i,value in ipairs(parameters) do text[i] = tostring(value); bound[i-1] = text[i] end
    result = pq.PQexecParams(connection,sql,#parameters,nil,bound,nil,nil,0)
  else result = pq.PQexec(connection,sql) end
  local status = pq.PQresultStatus(result)
  if status ~= 1 and status ~= 2 then local message=ffi.string(pq.PQresultErrorMessage(result));pq.PQclear(result);error(message) end
  return result
end
local function command(sql,parameters) pq.PQclear(query(sql,parameters)) end
local ok,error = pcall(function()
  local owner='PostgreSQL Lua'
  command('CREATE TABLE IF NOT EXISTS pg_demo_products(client VARCHAR,id INTEGER,name VARCHAR,price DECIMAL(18,2),stock INTEGER,PRIMARY KEY(client,id))')
  command('BEGIN')
  command('DELETE FROM pg_demo_products WHERE client=$1',{owner})
  command('INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)',{owner,1,'Coffee','19.99',3})
  command('INSERT INTO pg_demo_products VALUES($1,$2,$3,$4,$5)',{owner,2,'Rice','8.50',10})
  command('UPDATE pg_demo_products SET stock=stock+1 WHERE client=$1 AND id=$2',{owner,1})
  command('DELETE FROM pg_demo_products WHERE client=$1 AND id=$2',{owner,2})
  command('COMMIT')
  local result=query('SELECT json_array(id,name,CAST(price AS VARCHAR),stock) FROM pg_demo_products WHERE client=$1 ORDER BY id',{owner})
  assert(pq.PQntuples(result)==1,'Unexpected row count')
  local row=ffi.string(pq.PQgetvalue(result,0,0));pq.PQclear(result)
  assert(row=='[1,"Coffee","19.99",4]',row)
  print('{"passed":true,"client":"PostgreSQL Lua","driver":"libpq via LuaJIT FFI","rows":['..row..']}')
end)
pq.PQfinish(connection)
if not ok then io.stderr:write(tostring(error)..'\n');os.exit(1) end

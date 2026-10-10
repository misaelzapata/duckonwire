-- Run in psql connected to your Neon database as its owner.
-- Connection variables can be set in the session or loaded from environment.
-- Never pass the reader password through -v, connection URLs or public files.
\set ON_ERROR_STOP on
\set ECHO none

\if :{?duckonwire_host}
\else
\getenv duckonwire_host DUCKONWIRE_FDW_HOST
\endif
\if :{?duckonwire_port}
\else
\getenv duckonwire_port DUCKONWIRE_FDW_PORT
\endif
\if :{?duckonwire_port}
\else
\set duckonwire_port 9496
\endif
\if :{?duckonwire_database}
\else
\getenv duckonwire_database DUCKONWIRE_FDW_DATABASE
\endif
\if :{?duckonwire_database}
\else
\set duckonwire_database duckonwire
\endif
\if :{?duckonwire_user}
\else
\getenv duckonwire_user DUCKONWIRE_FDW_USER
\endif
\if :{?duckonwire_user}
\else
\set duckonwire_user reader
\endif
\if :{?duckonwire_password}
\else
\getenv duckonwire_password DUCKONWIRE_FDW_PASSWORD
\endif
\if :{?duckonwire_sslrootcert}
\else
\getenv duckonwire_sslrootcert DUCKONWIRE_FDW_SSLROOTCERT
\endif
\if :{?duckonwire_sslrootcert}
\else
\set duckonwire_sslrootcert system
\endif

\if :{?duckonwire_host}
\else
\echo 'Set duckonwire_host or DUCKONWIRE_FDW_HOST before setup.'
\quit 2
\endif
\if :{?duckonwire_password}
\else
\echo 'Supply the reader password privately before setup.'
\quit 2
\endif

-- Deliberately fail on existing names instead of changing another mapping.
BEGIN;
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
CREATE SERVER duckonwire
  FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (
    host :'duckonwire_host',
    port :'duckonwire_port',
    dbname :'duckonwire_database',
    sslmode 'verify-full',
    sslrootcert :'duckonwire_sslrootcert',
    updatable 'false',
    truncatable 'false',
    keep_connections 'off',
    fetch_size '1000'
  );
CREATE USER MAPPING FOR CURRENT_USER
  SERVER duckonwire
  OPTIONS (user :'duckonwire_user', password :'duckonwire_password');
CREATE SCHEMA duckonwire;
CREATE FOREIGN TABLE duckonwire.products (
  id integer,
  name text,
  brand text,
  category text,
  size text,
  unit text,
  country_code text
) SERVER duckonwire OPTIONS (schema_name 'main', table_name 'products');
CREATE FOREIGN TABLE duckonwire.prices (
  id integer,
  product_id integer,
  price numeric(18,2),
  day date
) SERVER duckonwire OPTIONS (schema_name 'main', table_name 'prices');
COMMIT;
\unset duckonwire_password

-- Remove only the Neon objects created by setup.sql; remote data stays intact.
-- No CASCADE: unexpected tables or mappings cause a rollback instead.
\set ON_ERROR_STOP on
BEGIN;
DROP FOREIGN TABLE duckonwire.prices;
DROP FOREIGN TABLE duckonwire.products;
DROP USER MAPPING FOR CURRENT_USER SERVER duckonwire;
DROP SCHEMA duckonwire;
DROP SERVER duckonwire;
COMMIT;

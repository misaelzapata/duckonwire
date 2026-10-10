-- Queries run in Neon; postgres_fdw reads the protected DuckOnWire catalog.
\set ON_ERROR_STOP on

SELECT id, name, brand, country_code
FROM duckonwire.products
ORDER BY id
LIMIT 5;

SELECT country_code, count(*) AS products
FROM duckonwire.products
GROUP BY country_code
ORDER BY country_code;

WITH latest_price AS (
  SELECT product_id, price, day,
         row_number() OVER (PARTITION BY product_id ORDER BY day DESC, id DESC) AS position
  FROM duckonwire.prices
  WHERE product_id <= 100
)
SELECT p.id, p.name, p.brand, p.country_code, r.price, r.day
FROM duckonwire.products AS p
JOIN latest_price AS r ON r.product_id = p.id AND r.position = 1
WHERE p.id <= 100
ORDER BY p.id
LIMIT 10;

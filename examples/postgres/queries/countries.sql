-- Products by country. Count the catalog's two markets.
SELECT country_code, count(*) AS products
FROM products GROUP BY country_code ORDER BY country_code;

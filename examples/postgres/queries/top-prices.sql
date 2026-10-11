-- Three highest price observations in each market; demo prices are synthetic.
-- The demo schema limits prices to 0.50..250.00; DECIMAL(9,2) keeps cents exact.
-- One cent outweighs every INTEGER product id, preserving the id tie break.
WITH grouped AS (
  SELECT p.country_code,
    arg_max(pr.id,
      (CASE WHEN pr.price IS NULL THEN -1000000000::BIGINT
            ELSE CAST(CAST(pr.price AS DECIMAL(9,2))*100::DECIMAL(3,0) AS BIGINT) END)
      *4294967296::BIGINT - CAST(p.id AS BIGINT), 3) AS chosen
  FROM products p JOIN prices pr ON pr.product_id = p.id
  GROUP BY p.country_code
), expanded AS (
  SELECT country_code, unnest(chosen) AS item FROM grouped
)
SELECT e.country_code, p.name AS name, p.brand AS brand, pr.price AS price
FROM expanded e JOIN prices pr ON pr.id=e.item JOIN products p ON p.id=pr.product_id
ORDER BY e.country_code ASC NULLS LAST, price DESC NULLS LAST, name ASC NULLS LAST;

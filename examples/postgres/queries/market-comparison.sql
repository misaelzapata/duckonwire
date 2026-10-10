-- Market comparison. Compare each sample with its whole-market average.
-- Demo prices are synthetic.
WITH market_average AS (
  SELECT p.country_code, round(avg(pr.price), 2) AS market_average
  FROM products p JOIN prices pr ON pr.product_id = p.id
  GROUP BY p.country_code
)
SELECT p.name, p.brand, p.country_code, pr.price, a.market_average
FROM products p JOIN prices pr ON pr.product_id = p.id
JOIN market_average a ON a.country_code IS NOT DISTINCT FROM p.country_code
ORDER BY p.id, pr.id;

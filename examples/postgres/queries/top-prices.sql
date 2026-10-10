-- Top prices by country. Three highest price observations in each market.
-- Demo prices are synthetic.
WITH grouped AS (
  SELECT p.country_code,
    arg_max(struct_pack(product_id := p.id, price := pr.price),
      struct_pack(has_price := pr.price IS NOT NULL, price := pr.price,
                  reverse_id := -CAST(p.id AS BIGINT)), 3) AS chosen
  FROM products p JOIN prices pr ON pr.product_id = p.id
  GROUP BY p.country_code
), expanded AS (
  SELECT country_code, unnest(chosen) AS item FROM grouped
)
SELECT e.country_code, p.name AS name, p.brand AS brand, e.item.price AS price
FROM expanded e JOIN products p ON p.id = e.item.product_id
ORDER BY e.country_code ASC NULLS LAST, price DESC NULLS LAST, name ASC NULLS LAST;

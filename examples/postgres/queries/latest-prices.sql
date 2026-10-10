-- Latest prices. A join with the newest sample price per product.
-- Demo prices are synthetic.
WITH latest AS (
  SELECT product_id,
    arg_max(struct_pack(price := price, day := day),
      struct_pack(has_day := day IS NOT NULL, day := day, id := id)) AS item
  FROM prices WHERE product_id <= 500
  GROUP BY product_id
)
SELECT p.id, p.name, p.brand, r.item.price AS price, r.item.day AS day
FROM products p JOIN latest r ON r.product_id = p.id
WHERE p.id <= 500 ORDER BY p.id;

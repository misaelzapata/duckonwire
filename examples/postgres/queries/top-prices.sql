-- Top three prices per country, with product IDs breaking equal-price ties.
-- The demo prices are synthetic; NULL prices sort last.
WITH grouped AS (
    SELECT
        p.country_code,
        arg_max(
            struct_pack(name := p.name, brand := p.brand, price := pr.price),
            struct_pack(
                has_price := pr.price IS NOT NULL,
                price := pr.price,
                reverse_id := -CAST(p.id AS BIGINT)
            ),
            3
        ) AS chosen
    FROM products p
    JOIN prices pr ON pr.product_id = p.id
    GROUP BY p.country_code
), expanded AS (
    SELECT country_code, unnest(chosen) AS item
    FROM grouped
)
SELECT
    country_code,
    item.name AS name,
    item.brand AS brand,
    item.price AS price
FROM expanded
ORDER BY
    country_code ASC NULLS LAST,
    price DESC NULLS LAST,
    name ASC NULLS LAST;

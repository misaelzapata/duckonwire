-- See sample prices. Generated prices, not retail offers.
SELECT p.name, p.brand, p.country_code, pr.price, pr.day
FROM products p
JOIN prices pr ON pr.product_id = p.id
ORDER BY p.id, pr.id;

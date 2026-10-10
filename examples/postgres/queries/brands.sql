-- Compare brands. Count price observations and average them by brand.
SELECT p.country_code, p.brand, count(*) AS observations,
       round(avg(pr.price), 2) AS average_price
FROM products p JOIN prices pr ON pr.product_id = p.id
GROUP BY p.country_code, p.brand
ORDER BY observations DESC, p.brand, p.country_code;

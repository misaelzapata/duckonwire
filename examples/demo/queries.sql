-- Public basic products; country_code is the national catalog market.
SELECT id,name,brand,category,size,unit,country_code
FROM products ORDER BY id LIMIT 20;

-- The displayed prices are seeded synthetic values, not real retail offers.
SELECT p.name,p.brand,p.country_code,p.size,p.unit,pr.price,pr.day
FROM products p JOIN prices pr ON pr.product_id=p.id
ORDER BY p.id LIMIT 20;

-- Average generated price per public manufacturer brand and market.
SELECT p.country_code,p.brand,count(*) AS products,avg(pr.price) AS average_price
FROM products p JOIN prices pr ON pr.product_id=p.id
GROUP BY p.country_code,p.brand ORDER BY p.country_code,p.brand;

-- Compare each generated price with the average in its catalog market.
SELECT p.name,p.brand,p.country_code,pr.price,
       avg(pr.price) OVER (PARTITION BY p.country_code) AS market_average
FROM products p JOIN prices pr ON pr.product_id=p.id
ORDER BY p.id LIMIT 20;

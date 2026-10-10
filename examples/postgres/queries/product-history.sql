-- Complete price history for the selected product, using the existing foreign-key index.
-- All selected observations reach LAG before paging. Demo prices are synthetic.
WITH selected_history AS MATERIALIZED (SELECT id,product_id,price,day FROM prices WHERE product_id=1), history AS (
 SELECT id,product_id,price,day,
        lag(price) OVER(PARTITION BY product_id ORDER BY day ASC NULLS LAST,id) AS previous_price
 FROM selected_history
)
SELECT h.id,p.name,p.brand,h.day,h.price,h.previous_price,
       round(h.price-h.previous_price,2) AS price_change
FROM history h JOIN products p ON p.id=h.product_id
ORDER BY h.day ASC NULLS LAST,h.id;

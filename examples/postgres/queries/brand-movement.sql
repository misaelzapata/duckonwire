-- Compare the first and latest price of every product, grouped by brand.
-- The native numeric key preserves DATE/observation-id order, including NULLs.
-- Demo prices are synthetic.
WITH keyed AS (
 SELECT product_id,price,
   CAST(CASE WHEN day IS NULL THEN 2147483648
             WHEN day=DATE 'infinity' THEN 2147483647
             WHEN day=DATE '-infinity' THEN -2147483647
             ELSE date_diff('day',DATE '1970-01-01',day) END AS HUGEINT)
     *18446744073709551616::HUGEINT+id::HUGEINT AS first_key,
   CAST(CASE WHEN day IS NULL THEN -2147483648
             WHEN day=DATE 'infinity' THEN 2147483647
             WHEN day=DATE '-infinity' THEN -2147483647
             ELSE date_diff('day',DATE '1970-01-01',day) END AS HUGEINT)
     *18446744073709551616::HUGEINT+id::HUGEINT AS last_key
 FROM prices
), samples AS (
 SELECT product_id,
   arg_min(struct_pack(price:=price),first_key) AS first_sample,
   arg_max(struct_pack(price:=price),last_key) AS last_sample
 FROM keyed GROUP BY product_id
)
SELECT p.country_code,p.brand,count(*) AS products_with_history,
       round(avg(s.last_sample.price-s.first_sample.price),2) AS average_change,
       count_if(s.last_sample.price>s.first_sample.price) AS products_increased
FROM samples s JOIN products p ON p.id=s.product_id
GROUP BY p.country_code,p.brand
ORDER BY average_change DESC NULLS LAST,p.country_code ASC NULLS LAST,p.brand ASC NULLS LAST;

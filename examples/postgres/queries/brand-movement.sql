-- Changes by brand. Compare the first and latest price of each product.
-- Demo prices are synthetic.
WITH samples AS (
 SELECT product_id,
  arg_min(struct_pack(price:=price,day:=day),
          struct_pack(missing_day:=day IS NULL,day:=day,id:=id)) AS first_sample,
  arg_max(struct_pack(price:=price,day:=day),
          struct_pack(has_day:=day IS NOT NULL,day:=day,id:=id)) AS last_sample
 FROM prices GROUP BY product_id
)
SELECT p.country_code,p.brand,count(*) AS products_with_history,
       round(avg(s.last_sample.price-s.first_sample.price),2) AS average_change,
       count_if(s.last_sample.price>s.first_sample.price) AS products_increased
FROM samples s JOIN products p ON p.id=s.product_id
GROUP BY p.country_code,p.brand
ORDER BY average_change DESC NULLS LAST,p.country_code ASC NULLS LAST,p.brand ASC NULLS LAST;

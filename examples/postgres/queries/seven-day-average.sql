-- Seven-day market trends. A calendar-week average weighted by price observations.
-- Demo prices are synthetic.
WITH daily AS (
 SELECT p.country_code,pr.day,count(*) AS samples,
        sum(pr.price) AS price_total,count(pr.price) AS priced_samples,
        avg(pr.price) AS daily_average
 FROM products p JOIN prices pr ON pr.product_id=p.id
 GROUP BY p.country_code,pr.day
), rolling AS (
 SELECT country_code,day,samples,daily_average,
        sum(price_total) OVER week /
        nullif(sum(priced_samples) OVER week,0) AS seven_day_average
 FROM daily
 WINDOW week AS (PARTITION BY country_code ORDER BY day
                 RANGE BETWEEN INTERVAL 6 DAY PRECEDING AND CURRENT ROW)
)
SELECT country_code,day,samples,round(daily_average,2) AS daily_average,
       round(seven_day_average,2) AS seven_day_average
FROM rolling ORDER BY country_code ASC NULLS LAST,day;

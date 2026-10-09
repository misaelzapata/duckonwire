-- Bind one BIGINT parameter, for example 100, through the driver's parameter API.
WITH products AS (
    SELECT i::INTEGER AS id,i*2 AS amount FROM range($1::BIGINT) t(i)
), ranked AS (
    SELECT p.id,p.amount,ROW_NUMBER() OVER(ORDER BY p.id DESC) AS position
    FROM products p JOIN products q ON p.id=q.id
)
SELECT count(*)::BIGINT AS count,sum(amount)::BIGINT AS total
FROM ranked WHERE position<=5;

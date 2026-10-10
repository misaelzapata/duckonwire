-- Import products into a new schema first; prices must be empty.
-- These are reproducible synthetic prices, not real retail offers.
INSERT INTO prices
SELECT i::INTEGER AS id,
       (1 + (i - 1) % 46462)::INTEGER AS product_id,
       ((50 + (i * 48271 + 12345) % 24951) * 0.01)::DECIMAL(18,2) AS price,
       DATE '2026-01-01'
           + (((i * 1103515245 + 12345) % 2147483648) % 30)::INTEGER AS day
FROM range(1, 1000001) AS generated(i);

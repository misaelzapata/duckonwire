-- Execute schema.sql first, then these statements from the package root.
-- Only public product fields and reproducible synthetic prices are imported.
COPY products FROM 'examples/demo/products.csv'
    (FORMAT CSV, HEADER TRUE, DELIMITER ',', QUOTE '"', ESCAPE '"',
     FORCE_NOT_NULL(category,size,unit));
COPY prices FROM 'examples/demo/prices.csv' (FORMAT CSV, HEADER TRUE);

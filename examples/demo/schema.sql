CREATE TABLE products (
    id INTEGER PRIMARY KEY,
    name VARCHAR NOT NULL CHECK (length(name)>0),
    brand VARCHAR NOT NULL CHECK (length(brand)>0),
    category VARCHAR NOT NULL,
    size VARCHAR NOT NULL,
    unit VARCHAR NOT NULL,
    country_code VARCHAR NOT NULL CHECK (country_code IN ('AR','ES'))
);

CREATE TABLE prices (
    id INTEGER PRIMARY KEY,
    product_id INTEGER NOT NULL UNIQUE REFERENCES products(id),
    price DECIMAL(18,2) NOT NULL CHECK (price BETWEEN 0.50 AND 250.00),
    day DATE NOT NULL
);

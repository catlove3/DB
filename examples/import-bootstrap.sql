CREATE DATABASE import_demo;
USE import_demo;
CREATE TABLE student (id INT NOT NULL, name VARCHAR(32) NOT NULL, age INT, gpa FLOAT, PRIMARY KEY (id));
ALTER TABLE student ADD INDEX idx_age (age);

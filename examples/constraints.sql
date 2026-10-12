INSERT INTO student VALUES (1,'Other',18,2.00);
SELECT id,gpa FROM student WHERE id=1;
INSERT INTO enrollment VALUES (999,101,75.00);
DELETE FROM student WHERE id=1;
UPDATE student SET id=3 WHERE id=1;
ALTER TABLE student ADD PRIMARY KEY (id);
SELECT id,name FROM student WHERE id=1;
SELECT id,name FROM student WHERE id=3;

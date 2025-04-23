all : serialize

linux : serialize.so

serialize.so : serialize.c
	gcc -Wall -g -o $@ -fPIC --shared $^ -I./lua -lm

serialize : serialize.c
	gcc -Wall -g -o $@.dll --shared $^ -I./lua -lm

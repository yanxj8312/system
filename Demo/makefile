%.o:%.c
	gcc -c -o $@ $^
objfiles = test1.o test2.o
test.bin:$(objfiles)
	gcc -o $@ $^
all:test.bin
	@echo "complie done"

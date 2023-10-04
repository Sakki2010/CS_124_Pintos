CFLAGS = -Wall -Wextra -g -fsanitize=address,undefined
LDFLAGS = -lreadline

OBJS = mysh.o lexer.o tokenizer.o err_utils.o

all: mysh

mysh: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f *.o *~ mysh

.PHONY: all clean


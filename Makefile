CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter
# TODO: enable unused parameter warning

lapocket: lapocket.c
	$(CC) $(CFLAGS) -o lapocket lapocket.c

clean:
	rm -rf *.o lapocket

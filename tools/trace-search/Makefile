LDFLAGS=-lcapstone

all: tsearch

tsearch: tsearch.o
	$(CC) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $< -o $@

.PHONY: clean
clean:
	rm -f *.o
	rm -f tsearch

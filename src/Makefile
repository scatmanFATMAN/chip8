app=chip8
obj=main.o
cc=gcc
cflags=-Wall -g -D_GNU_SOURCE
libs=-lncurses -lpthread

all: $(app)

release: cflags:=$(filter-out -g, $(cflags))
release: $(app)

$(app): $(obj)
	$(cc) -o $@ $^ $(libs)

%.o: %.c
	$(cc) -o $@ -c $< $(cflags)

clean:
	rm -f $(obj) $(app)

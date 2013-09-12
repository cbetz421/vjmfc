CFLAGS := -O0 -ggdb -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -Wl,--as-needed

override CFLAGS += -Wmissing-prototypes -ansi -std=gnu99 -D_GNU_SOURCE

all:

vjmfc: main.o
bins += vjmfc

all: $(bins)

%.o:: %.c
	$(CC) $(CFLAGS) -MMD -o $@ -c $<

$(bins):
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(bins) *.o *.d

-include *.d

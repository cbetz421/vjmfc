CFLAGS := -O0 -ggdb -Wall -Wextra -Wno-unused-parameter
LDFLAGS := -Wl,--as-needed

override CFLAGS += -Wmissing-prototypes -ansi -std=gnu99 -D_GNU_SOURCE

CFLAGS += $(shell pkg-config --cflags libavformat libavcodec)
LIBS += $(shell pkg-config --libs libavformat libavcodec)

all:

vjmfc: main.o v4l2_mfc.o av.o dev.o
bins += vjmfc

all: $(bins)

%.o:: %.c
	$(CC) $(CFLAGS) -MMD -o $@ -c $<

$(bins):
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(bins) *.o *.d

-include *.d

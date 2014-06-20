
LIBPURPLE_CFLAGS += $(shell pkg-config --cflags glib-2.0 json-glib-1.0 purple nss gnome-keyring-1)
LIBPURPLE_LIBS += $(shell pkg-config --libs glib-2.0 json-glib-1.0 purple nss)

.PHONY:	all clean install

all: voipms.so

clean:
	rm *.so

%.so: %.c
	$(CC) $(CFLAGS) $(LIBPURPLE_CFLAGS) $(LIBPURPLE_LIBS) -o $@ $< -shared


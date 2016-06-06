
CFLAGS=-g


all:	jack_cat

jack_cat:	jack_cat.o
	$(CC) $(CFLAGS) -o jack_cat $< $$(pkg-config --libs jack) -lpthread



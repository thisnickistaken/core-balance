prefix=/usr
bin=$(prefix)/bin

#opts=-D_NO_THREADS
#libs=
opts=
libs=-lpthread

all:
	$(CC) -c core-balance.c -o core-balance.o $(opts)
	$(CC) core-balance.o -o core-balance $(libs)

install:
	install -m 755 -o root -g root core-balance $(bin)/

uninstall:
	rm -f $(bin)/core-balance

clean:
	rm -f *.o core-balance

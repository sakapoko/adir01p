CC      = cc
CFLAGS = -O2
OBJS = adir01p.o

all: adir01p

adir01p: $(OBJS)
	$(CC) $(LDFLAGS) -Wall -o $@ $(OBJS) -lusb-1.0 

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	@rm -f $(OBJS)
	@rm -f adir01p

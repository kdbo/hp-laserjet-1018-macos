CC       = clang
CFLAGS   = -Wall -O2 -Ifoo2zjs
LDFLAGS  = -lcups -lcupsimage

JBIG_SRC = foo2zjs/jbig.c foo2zjs/jbig_ar.c

FILTERS  = rastertoxqx rastertozjs rastertohiperc rastertoqpdl \
           rastertolava rastertohbpl2 rastertohp rastertooak rastertoslx

TOOLS    = arm2hpdl xqxdecode

all: $(FILTERS)

tools: $(TOOLS)

rastertoxqx: rastertoxqx.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertozjs: rastertozjs.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertohiperc: rastertohiperc.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertoqpdl: rastertoqpdl.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertolava: rastertolava.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertohbpl2: rastertohbpl2.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertohp: rastertohp.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertooak: rastertooak.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

rastertoslx: rastertoslx.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

arm2hpdl: foo2zjs/arm2hpdl.c
	$(CC) $(CFLAGS) -o $@ $<

xqxdecode: foo2zjs/xqxdecode.c $(JBIG_SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(FILTERS) $(TOOLS)

.PHONY: all tools clean

LIBUSB_CFLAGS += -Wall $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LDFLAGS += $(shell pkg-config --libs libusb-1.0)
CFLAGS = -g -O3 #-Wall

TOOLS = sendts-it9507

all: $(TOOLS)

sendts-it9507: CFLAGS+=$(LIBUSB_CFLAGS)
sendts-it9507: LDFLAGS+=$(LIBUSB_LDFLAGS)

%: %.c
	gcc $(CFLAGS) $< -o $@  $(LDFLAGS)

clean:
	rm -f $(TOOLS)

CC = riscv64-linux-gnu-gcc

CFLAGS = -fno-link-libatomic -march=rv64i -mabi=lp64 -gdwarf-5 -static-pie -fPIC --sysroot=$(HOME)/sysroot

TARGET = shell

SRCS = shell.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
        $(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
        $(CC) $(CFLAGS) -c $< -o $@

clean:
        rm -f $(OBJS) $(TARGET)

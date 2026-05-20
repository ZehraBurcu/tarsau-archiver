CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2
TARGET = tarsau
SRC = src/tarsau.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

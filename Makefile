# GCC options
CC = gcc
CFLAGS = -O3 -Ofast -march=native -fopenmp -std=c99 -pedantic -Wall -Wextra -g
#CFLAGS = -Kfast -std=c99 
LDFLAGS = -lm

export OMP_NUM_THREADS ?= 32

#Debug options
#CFLAGS = -g -Og -std=c99 -pedantic -fsanitize=undefined -fsanitize=address

# Intel icc compiler
#CC = icc
#CFLAGS = -restrict -Ofast -std=c99 -pedantic
#LDFLAGS =

# Clang options
#CC = clang
#CFLAGS = -Ofast -std=c99 -pedantic
#LDFLAGS = -lm


SOURCE = current.c emf.c particles.c random.c timer.c main.c simulation.c zdf.c

TARGET = zpic

DOCSBASE = docs

DOCS = $(DOCSBASE)/html/index.html

OBJ = $(SOURCE:.c=.o)

all : $(SOURCE) $(TARGET)

docs : $(DOCS)

$(TARGET) : $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $(TARGET)

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

$(DOCS) : $(SOURCE)
	@doxygen ./Doxyfile

clean:
	@touch $(TARGET) $(OBJ)
	rm -f $(TARGET) $(OBJ)
	rm -rf $(DOCSBASE)

run: $(TARGET)
	@echo "A correr com OMP_NUM_THREADS=$(OMP_NUM_THREADS)"
	./$(TARGET)

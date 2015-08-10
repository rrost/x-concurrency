CXX=g++
CPPFLAGS=-O3 -std=c++11 -Wall -pedantic -I"$(BOOST)"
LIB=-lpthread
BINDIR=./bin
OBJDIR=./obj
BIN=$(BINDIR)/spsc_queue_ut
SRC=$(wildcard *.cpp)
OBJ=$(patsubst %, $(OBJDIR)/%, $(patsubst %.cpp, %.o, $(SRC)))

all: makedirs $(OBJ)
	$(CXX) -o $(BIN) $(OBJ) $(LIB)

$(OBJ): obj/%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

.PHONY: makedirs clean

makedirs:
	@mkdir -p $(BINDIR) $(OBJDIR)

clean:
	@rm -f $(OBJDIR)/*.o
	@rm -f $(BIN)

CXX=g++
CPPFLAGS=-O3 -std=c++11 -Wall -pedantic
BINDIR=./bin
OBJDIR=./obj
SRC=%.cpp
BIN=$(BINDIR)/spsc_queue_ut
OBJ=$(OBJDIR)/%.o

all: $(OBJ) $(BINDIR)
	$(CXX) -o $(BIN) $^

.PHONY $(OBJDIR):
$(OBJDIR):
	mkdir $(OBJDIR)

.PHONY $(BINDIR)
$(BINDIR):
	mkdir $(BINDIR)

$(OBJ): $(SRC) $(OBJDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $<

.PHONY clean
clean:
	rm -f $(OBJDIR)/*.o
	rm -f $(BIN)

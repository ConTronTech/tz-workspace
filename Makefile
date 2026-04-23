CXX        ?= g++
PKG_CONFIG ?= pkg-config
PKGS       := gtkmm-4.0 sqlite3

CXXFLAGS  += -std=c++17 -Wall -Wextra -Wpedantic -O2 \
             -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
             -fPIE -MMD -MP
CXXFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PKGS))

LDFLAGS   += -Wl,-z,relro,-z,now -pie
LIBS      := $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC := src/main.cpp src/MainWindow.cpp src/TzService.cpp src/Db.cpp
OBJ := $(SRC:.cpp=.o)
DEP := $(OBJ:.o=.d)
BIN := tz-workspace

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJ) $(DEP) $(BIN)

-include $(DEP)

.PHONY: all run clean

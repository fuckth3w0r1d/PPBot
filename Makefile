CXX = g++
CXXFLAGS = -std=c++17 -O2 -Iinclude
LDFLAGS = -lssl -lcrypto -lpthread

SRCS := $(shell find src -name "*.cpp")
OBJS := $(SRCS:.cpp=.o)

TARGET = release/bot

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
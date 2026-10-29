# PPBot Makefile

# 编译器和参数
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Iinclude
LDFLAGS = -lssl -lcrypto -lpthread -lsqlite3

# 源文件
SRCS := $(shell find src -name "*.cpp")
# 对应对象文件，保持目录结构
OBJS := $(patsubst src/%.cpp,release/%.o,$(SRCS)) release/sqlite-vec.o
# 依赖文件
DEPS := $(OBJS:.o=.d)

# 目标文件
TARGET = release/bot
TEST_TARGET = release/tests/chat_test
LOG_TEST_TARGET = release/tests/logging_test
ARCHIVE_TEST_TARGET = release/tests/archive_test
STOP_TEST_TARGET = release/tests/stop_test
TEST_OBJS = $(filter-out release/main.o,$(OBJS))

# 默认目标
all: $(TARGET)

test: $(TEST_TARGET) $(LOG_TEST_TARGET) $(ARCHIVE_TEST_TARGET) $(STOP_TEST_TARGET)
	./$(TEST_TARGET)
	./$(LOG_TEST_TARGET)
	./$(ARCHIVE_TEST_TARGET)
	./$(STOP_TEST_TARGET)

$(TEST_TARGET): tests/chat_test.cpp $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(LOG_TEST_TARGET): tests/logging_test.cpp $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(ARCHIVE_TEST_TARGET): tests/archive_test.cpp $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(STOP_TEST_TARGET): tests/stop_test.cpp src/main.cpp $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ tests/stop_test.cpp $(TEST_OBJS) $(LDFLAGS)

release/sqlite-vec.o: src/sqlite-vec.c include/sqlite-vec.h Makefile
	@mkdir -p $(dir $@)
	$(CC) -O2 -fno-math-errno -DSQLITE_CORE -DSQLITE_VEC_STATIC -DSQLITE_VEC_OMIT_FS -Iinclude -c $< -o $@

# 链接目标
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则：保持 release 目录结构
release/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
	# 自动生成依赖文件
	$(CXX) $(CXXFLAGS) -MM $< -MF $(patsubst src/%.cpp,release/%.d,$<) -MT $@

# 自动包含依赖文件
-include $(DEPS)

# 清理
clean:
	rm -rf release

.PHONY: all clean test

# PPBot Makefile

# 编译器和参数
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Iinclude
LDFLAGS = -lssl -lcrypto -lpthread

# 源文件
SRCS := $(shell find src -name "*.cpp")
# 对应对象文件，保持目录结构
OBJS := $(patsubst src/%.cpp,release/%.o,$(SRCS))
# 依赖文件
DEPS := $(OBJS:.o=.d)

# 目标文件
TARGET = release/bot

# 默认目标
all: $(TARGET)

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

.PHONY: all clean
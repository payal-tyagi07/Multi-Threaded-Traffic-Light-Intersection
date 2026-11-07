CXX := g++
CXXFLAGS := -Wall -pthread -std=c++11
TARGET := traffic_controller
SOURCE := main.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

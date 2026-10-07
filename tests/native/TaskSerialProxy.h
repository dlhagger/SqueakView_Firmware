#pragma once
#include <string>

class TaskSerialProxy {
public:
  std::string output;
  explicit TaskSerialProxy(MouseHouse*) {}
  void print(const char* value) { output += value; }
  void print(char* value) { output += value; }
  template <typename T> void print(T value) { output += std::to_string(value); }
  template <typename T> void println(T value) { print(value); output += '\n'; }
};

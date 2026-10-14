#include <stdexcept>
#include <typeinfo>
extern "C" void native_cpp_throw(const char *message) {throw std::runtime_error(message);}
extern "C" const std::type_info *native_cpp_exception_type() {return &typeid(std::runtime_error);}

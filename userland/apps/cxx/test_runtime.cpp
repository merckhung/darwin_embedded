#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <sys/utsname.h>

static std::atomic<unsigned> destroyed{0};
struct Guard { ~Guard() { ++destroyed; } };
struct Base { virtual ~Base() = default; };
struct Derived : Base { int value=73; };
static thread_local int thread_value=11;
extern "C" void native_cpp_throw(const char *message);
extern "C" const std::type_info *native_cpp_exception_type();
#define CHECK(c,label) do {if (!(c)) {std::fprintf(stderr,"Native C++ runtime failure: %s\n",label);return 1;}} while(0)
extern "C" int main(int argc,char **argv)
{
    CHECK(argc==2 && std::strlen(argv[1])==48,"nonce");
    std::string message="native-"+std::string(argv[1]);
    std::vector<int> values{5,1,3};std::sort(values.begin(),values.end());
    CHECK((values==std::vector<int>{1,3,5}),"original-vector-and-sort");
    const char decimal[]="3.125";double number=0;auto parsed=std::from_chars(decimal,decimal+5,number);
    CHECK(parsed.ec==std::errc{} && number==3.125,"original-floating-from-chars");
    CHECK(std::regex_match(message,std::regex("native-[0-9a-f]{48}")),"original-regex");
    Derived derived;Base *base=&derived;
    CHECK(dynamic_cast<Derived *>(base)==&derived && typeid(*base)==typeid(Derived),"original-rtti");
    bool caught=false;
    std::printf("NATIVE_CXX_TYPE:%p:%p:%s:%s\n",(const void *)&typeid(std::runtime_error),(const void *)native_cpp_exception_type(),typeid(std::runtime_error).name(),native_cpp_exception_type()->name());std::fflush(stdout);
    try { Guard guard;native_cpp_throw(message.c_str()); }
    catch (const std::runtime_error &error) {caught=message==error.what();}
    catch (...) {std::fprintf(stderr,"Native C++ runtime failure: typed-exception-catch-missed\n");return 1;}
    CHECK(caught && destroyed==1,"real-cross-image-exception-and-unwinding");
    std::promise<int> promise;auto future=promise.get_future();
    std::thread worker([&] {thread_value=29;promise.set_value(thread_value);});
    CHECK(future.wait_for(std::chrono::seconds(5))==std::future_status::ready && future.get()==29,"native-thread-promise-and-timed-wait");
    worker.join();CHECK(thread_value==11,"native-cxx-thread-local-isolation");
    auto directory=std::filesystem::path("/tmp")/("native-cxx-runtime-"+std::string(argv[1]));
    CHECK(std::filesystem::create_directory(directory),"real-filesystem-directory");
    {std::ofstream file(directory/"input");file<<message;CHECK(file.good(),"original-file-stream");}
    CHECK(std::filesystem::copy_file(directory/"input",directory/"output"),"original-stream-copy-backend");
    std::string copied;{std::ifstream file(directory/"output");file>>copied;}
    CHECK(copied==message && std::filesystem::remove_all(directory)==3,"real-file-copy-and-removal");
    struct utsname identity;CHECK(!uname(&identity),"uname");
    std::printf("NATIVE_CXX_RUNTIME_OK:%s:%s:%s\n",argv[1],identity.release,identity.machine);return 0;
}

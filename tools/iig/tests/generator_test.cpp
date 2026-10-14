#include "ast.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

int main() {
  const std::string definition = R"(
#define kReasonCapacity 4096
class KERNEL TestServer : public OSObject {
public:
  virtual kern_return_t Panic(const char reason[kReasonCapacity]);
  virtual kern_return_t Exit(const char reason[128]);
  static kern_return_t Create(TestServer **server);
};
)";
  iig::File file;
  std::string error;
  require(iig::parseIigFile("TestServer.iig", definition, file, error),
          error.c_str());
  require(file.classes.size() == 1, "expected one class");
  require(file.classes[0].methods.size() == 3, "RPC methods were lost");
  const auto &panic = file.classes[0].methods[0];
  require(panic.params.size() == 1, "expected one Panic parameter");
  require(panic.params[0].name == "reason", "array bound became the parameter name");
  require(panic.params[0].arrayCount == 4096, "symbolic array capacity was lost");
  require(file.classes[0].methods[1].params[0].arrayCount == 128,
          "literal array capacity was lost");

  std::string impl;
  iig::generateImpl(file, impl);
  require(impl.find("char __reason[4096]") != std::string::npos,
          "bounded RPC payload did not preserve the declared capacity");
  require(impl.find("rpc.kernelContent = &msg->content.__hdr;") != std::string::npos,
          "outgoing RPC did not initialize the current kernel ABI");
  require(impl.find("IORPCMessage * msg = rpc.kernelContent;") != std::string::npos,
          "dispatch did not read the current kernel ABI");
  require(impl.find("if (!msg) return kIOReturnIPCError;") != std::string::npos,
          "null kernel RPC content must be rejected");

  iig::File invalid;
  error.clear();
  require(!iig::parseIigFile("Invalid.iig",
          "class KERNEL Invalid : public OSObject { virtual kern_return_t "
          "Panic(const char reason[MissingCapacity]); };", invalid, error),
          "an unresolved array bound must fail rather than generate a scalar payload");
  require(error.find("MissingCapacity") != std::string::npos,
          "unresolved array diagnostic must identify the bound");

  iig::File native;
  error.clear();
  require(iig::parseIigFile("Native.iig",
          "class NATIVE KERNEL Concrete : public OSObject { virtual void "
          "Ready(OSAction *action TARGET) LOCAL = 0; static kern_return_t "
          "Create(uint64_t value, Concrete **instance) LOCAL; }; "
          "class NATIVE KERNEL Abstract : public OSObject { virtual void "
          "Local() LOCALONLY = 0; }; "
          "class KERNEL Callback : public OSObject { virtual void "
          "Ready(OSAction *action TARGET) LOCAL = 0; virtual void "
          "Complete(OSAction *action TARGET) KERNEL TYPE(Callback::Ready); };",
          native, error), error.c_str());
  iig::generateImpl(native, impl);
  require(impl.find("OSDefineMetaClassAndStructors(Concrete,") != std::string::npos,
          "a remote pure virtual callback must retain kernel constructors");
  require(impl.find("OSDefineMetaClassAndAbstractStructors(Abstract,") != std::string::npos,
          "a local-only pure virtual method must retain an abstract metaclass");
  require(impl.find("Concrete::Create_Invoke(rpc, &Concrete::Create_Impl)") != std::string::npos,
          "native local factory RPCs must dispatch to their kernel implementation");
  require(impl.find("Concrete::Create_Call(Concrete_Create_Args)") != std::string::npos,
          "native local factories must retain the exported call entry point");
  require(impl.find("OSDefineMetaClassAndStructors(OSAction_Callback_Complete, OSAction)") != std::string::npos,
          "kernel callbacks must define their action metaclass");
  require(impl.find("OSAction::CreateWithTypeName(this,") != std::string::npos,
          "kernel callbacks must construct the declared action type");
  std::string header;
  iig::generateHeader(native, header);
  require(header.find("class OSAction_Callback_Complete : public OSAction") != std::string::npos,
          "kernel callback action class must be declared");
  require(header.find("= 0;") != std::string::npos,
          "local-only pure virtual methods must remain abstract declarations");
  return 0;
}

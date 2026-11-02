#include "updater/update_platform.h"

#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "updater/update_paths.h"

namespace trg::update {
namespace {

template <typename T>
struct Owned {
  T ref = nullptr;
  ~Owned() {
    if (ref) {
      CFRelease(ref);
    }
  }
};

std::string Utf8(CFTypeRef value) {
  if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
    return {};
  }
  const CFStringRef text = static_cast<CFStringRef>(value);
  const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
  std::string result(size_t(size), '\0');
  if (!CFStringGetCString(text, result.data(), size, kCFStringEncodingUTF8)) {
    return {};
  }
  result.resize(std::char_traits<char>::length(result.c_str()));
  return result;
}

bool SigningOf(SecStaticCodeRef code, std::string& team, std::string& identifier) {
  Owned<CFDictionaryRef> info;
  if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info.ref) != errSecSuccess || !info.ref) {
    return false;
  }
  team = Utf8(CFDictionaryGetValue(info.ref, kSecCodeInfoTeamIdentifier));
  identifier = Utf8(CFDictionaryGetValue(info.ref, kSecCodeInfoIdentifier));
  return true;
}

// Both go into a requirement string, so only the characters Apple issues them with get through.
bool Plain(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool RunningAppSigning(std::string& team, std::string& identifier) {
  team.clear();
  identifier.clear();
  Owned<SecCodeRef> self;
  Owned<SecStaticCodeRef> code;
  if (SecCodeCopySelf(kSecCSDefaultFlags, &self.ref) != errSecSuccess ||
      SecCodeCopyStaticCode(self.ref, kSecCSDefaultFlags, &code.ref) != errSecSuccess) {
    return false;
  }
  return SigningOf(code.ref, team, identifier);
}

bool VerifyMacApp(const std::filesystem::path& app, std::string& error) {
  error.clear();
  std::string team;
  std::string identifier;
  if (!RunningAppSigning(team, identifier) || !Plain(team) || !Plain(identifier)) {
    error = "This copy is not signed with a Developer ID, so it cannot check an update.";
    return false;
  }
  const std::string path = Utf8Path(app);
  Owned<CFURLRef> url;
  url.ref = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                    CFIndex(path.size()), true);
  Owned<SecStaticCodeRef> code;
  if (!url.ref || SecStaticCodeCreateWithPath(url.ref, kSecCSDefaultFlags, &code.ref) != errSecSuccess) {
    error = "The update is not a signed app.";
    return false;
  }
  const std::string text = "anchor apple generic and certificate leaf[subject.OU] = \"" + team +
                           "\" and identifier \"" + identifier + "\"";
  Owned<CFStringRef> requirement_text;
  requirement_text.ref = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
  Owned<SecRequirementRef> requirement;
  if (!requirement_text.ref ||
      SecRequirementCreateWithString(requirement_text.ref, kSecCSDefaultFlags, &requirement.ref) != errSecSuccess) {
    error = "Cannot build the update's signing check.";
    return false;
  }
  const OSStatus status = SecStaticCodeCheckValidity(
      code.ref, kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate, requirement.ref);
  if (status != errSecSuccess) {
    error = "The update is not signed by this app's developer (" + std::to_string(int(status)) + ").";
    return false;
  }
  return true;
}

}  // namespace trg::update

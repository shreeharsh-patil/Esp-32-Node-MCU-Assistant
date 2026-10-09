#include <cassert>
#include <string>
#include "pocket_backend_policy.h"

int main() {
    assert(pocket::ValidBackendUrl("https://shreeharsh-assistant.vercel.app"));
    assert(pocket::ValidBackendUrl("https://my.server.example/"));
    const char* invalid[] = {"http://server.example",
                             "https://",
                             "https://localhost",
                             "https://user:password@server.example",
                             "https://server.example/ws",
                             "https://server.example?token=secret",
                             "https://server.example#fragment",
                             "https://server.example:443",
                             "https://server..example",
                             "https://-server.example",
                             "https://server-.example",
                             "https://server.example\r\nInjected: header",
                             "https://server.example//"};
    for (const char* value : invalid)
        assert(!pocket::ValidBackendUrl(value));
    const std::string long_label = "https://" + std::string(64, 'a') + ".example";
    assert(!pocket::ValidBackendUrl(long_label.c_str()));
    assert(!pocket::ValidBackendUrl(nullptr));
    assert(pocket::ValidDeviceToken(std::string(32, 'A').c_str()));
    assert(pocket::ValidDeviceToken(std::string(128, '_').c_str()));
    assert(!pocket::ValidDeviceToken(std::string(31, 'A').c_str()));
    assert(!pocket::ValidDeviceToken(std::string(129, 'A').c_str()));
    assert(!pocket::ValidDeviceToken((std::string(32, 'A') + "\r\nHeader").c_str()));
    assert(!pocket::ValidDeviceToken((std::string(32, 'A') + "Bearer ").c_str()));
    assert(!pocket::ValidDeviceToken(nullptr));
}

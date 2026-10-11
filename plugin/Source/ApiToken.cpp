#include "ApiToken.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <mutex>

#include "aicontact/Auth.h"

#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <sys/stat.h>
#endif

namespace {

constexpr const char* kKey = "apiToken";

struct Store {
    std::mutex mutex;
    juce::InterProcessLock processLock{"AIContactSettings"};
    std::unique_ptr<juce::PropertiesFile> props;
    juce::Time loadedAt;

    juce::PropertiesFile& file() {
        if (!props) {
            juce::PropertiesFile::Options o;
            o.applicationName = "AI Contact";
            o.filenameSuffix = ".settings";
           #if JUCE_LINUX || JUCE_BSD
            o.folderName = ".config/AI Contact";  // JUCE would otherwise create a visible ~/AI Contact
           #else
            o.folderName = "AI Contact";          // %APPDATA%\AI Contact, ~/Library/Application Support/AI Contact
           #endif
            o.osxLibrarySubFolder = "Application Support";
            o.processLock = &processLock;
            props = std::make_unique<juce::PropertiesFile>(o);
            loadedAt = props->getFile().getLastModificationTime();
        }
        // Another process (a second DAW, the Standalone app) may have rotated the token.
        const auto modified = props->getFile().getLastModificationTime();
        if (modified != loadedAt) {
            props->reload();
            loadedAt = modified;
        }
        return *props;
    }

    std::string save(const std::string& token) {
        auto& f = file();
        f.setValue(kKey, juce::String(token));
        f.saveIfNeeded();
       #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
        // The settings hold a secret: keep them private to the user.
        ::chmod(f.getFile().getFullPathName().toRawUTF8(), 0600);
       #endif
        loadedAt = f.getFile().getLastModificationTime();
        return token;
    }
};

Store& store() {
    static Store s;
    return s;
}

}  // namespace

namespace ApiToken {

std::string get() {
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::string token = s.file().getValue(kKey).toStdString();
    return aicontact::isValidToken(token) ? token : s.save(aicontact::generateToken());
}

std::string rotate() {
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.save(aicontact::generateToken());
}

}  // namespace ApiToken
